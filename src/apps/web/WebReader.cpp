#include "WebServer.h"

#include "DBQuery.h"
#include "WebTemplates.h"



struct EventThread {
    struct Event {
        defaultDb::environment::View_Event ev;
        tao::json::value json;

        flat_hash_set<std::string> children;
        uint64_t upVotes = 0;
        uint64_t downVotes = 0;
    };

    struct User {
        std::string pubkey;
        std::string username;
    };

    std::string id;
    bool found = false;
    flat_hash_map<std::string, Event> eventCache;
    flat_hash_map<std::string, User> userCache;


    EventThread(lmdb::txn &txn, Decompressor &decomp, std::string_view id) : id(std::string(id)) {
        auto existing = lookupEventById(txn, id);
        if (!existing) return;
        found = true;

        {
            tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, existing->primaryKeyId));
            eventCache.emplace(id, Event{ *existing, std::move(json) });
        }

        std::string prefix = "e";
        prefix += id;

        env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);
            if (parsedKey.s != prefix) return false;

            auto levId = lmdb::from_sv<uint64_t>(v);
            auto ev = lookupEventByLevId(txn, levId);

            tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));
            std::string id = std::string(sv(ev.flat_nested()->id()));

            eventCache.emplace(std::move(id), Event{ ev, std::move(json) });
            return true;
        });

        for (const auto &[id, e] : eventCache) {
            std::string parent;

            const auto &tags = e.json.at("tags").get_array();

            // Try to find an e-tag with a "reply" type
            for (const auto &t : tags) {
                const auto &tArr = t.get_array();
                if (tArr.at(0) == "e" && tArr.size() == 4 && tArr.at(3) == "reply") {
                    parent = from_hex(tArr.at(1).get_string());
                    break;
                }
            }

            if (!parent.size()) {
                // Otherwise, assume last e tag is reply

                for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
                    const auto &tArr = it->get_array();
                    if (tArr.at(0) == "e") {
                        parent = from_hex(tArr.at(1).get_string());
                        break;
                    }
                }
            }

            if (parent.size()) {
                auto p = eventCache.find(parent);
                if (p != eventCache.end()) {
                    auto &parent = p->second;

                    auto kind = e.ev.flat_nested()->kind();

                    if (kind == 1) {
                        parent.children.insert(id);
                    } else if (kind == 7) {
                        if (e.json.at("content").get_string() == "-") {
                            parent.downVotes++;
                        } else {
                            parent.upVotes++;
                        }
                    }
                }
            }
        }
    }

    TemplarResult render(lmdb::txn &txn, Decompressor &decomp) {
        if (!found) return TemplarResult{ "event not found" };

        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            auto p = eventCache.find(id);
            if (p == eventCache.end()) throw herr("unknown id");
            const auto &elem = p->second;

            struct {
                std::string comment;
                const Event *ev;
                User user;
                std::vector<TemplarResult> replies;
            } ctx;

            ctx.comment = elem.json.at("content").get_string();
            ctx.user = getUser(txn, decomp, sv(elem.ev.flat_nested()->pubkey()));
            ctx.ev = &elem;

            for (const auto &childId : elem.children) {
                ctx.replies.emplace_back(process(childId));
            }

            return tmpl::comment(ctx);
        };

        return process(id);
    }


    // FIXME: a lot of copying with return by value. maybe use std::unordered_map and return pointer?
    User getUser(lmdb::txn &txn, Decompressor &decomp, std::string_view pubkey) {
        auto u = userCache.find(pubkey);
        if (u != userCache.end()) return u->second;

        std::string username;

        std::string prefix;
        prefix += pubkey;
        prefix += lmdb::to_sv<uint64_t>(0);

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == 0) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));

                try {
                    auto content = tao::json::from_string(json.at("content").get_string());
                    username = content.at("name").get_string();
                } catch (std::exception &e) {
                }
            }

            return false;
        });

        if (username.size() == 0) username = to_hex(pubkey.substr(0,4));

        userCache.emplace(pubkey, User{ std::string(pubkey), username });
        return userCache[pubkey];
    }
};


void WebServer::runReader(ThreadPool<MsgReader>::Thread &thr) {
    Decompressor decomp;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReader::Request>(&newMsg.msg)) {
                LI << "GOT REQUEST FOR " << msg->url;

                auto respond = [&](const std::string &r){
                    std::string payload = "HTTP/1.0 200 OK\r\nContent-Length: ";
                    payload += std::to_string(r.size());
                    payload += "\r\n\r\n";
                    payload += r;

                    tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{msg->connId, msg->res, std::move(payload)}});
                    hubTrigger->send();
                };


                EventThread et(txn, decomp, from_hex("71f42777f35e4636dce9283b88ba9367f66e99b52e1123dc619ef6fcd9567df3"));

                auto body = et.render(txn, decomp);
                std::string html;

                {
                    struct {
                        TemplarResult body;
                    } ctx = {
                        body,
                    };

                    html = tmpl::main(ctx).str;
                }

                respond(html);
            }
        }
    }
}
