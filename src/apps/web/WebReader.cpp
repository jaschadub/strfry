#include "WebServer.h"

#include "DBQuery.h"
#include "WebTemplates.h"
#include "apps/web/uri.hh"



std::string renderTimestamp(uint64_t now, uint64_t ts) {
    uint64_t delta = now > ts ? now - ts : ts - now;

    const uint64_t A = 60;
    const uint64_t B = A*60;
    const uint64_t C = B*24;
    const uint64_t D = C*30.5;
    const uint64_t E = D*12;

    std::string output;

    if      (delta < B) output += std::to_string(delta / A) + " minutes";
    else if (delta < C) output += std::to_string(delta / B) + " hours";
    else if (delta < D) output += std::to_string(delta / C) + " days";
    else if (delta < E) output += std::to_string(delta / D) + " months";
    else                output += std::to_string(delta / E) + " years";

    if (now > ts) output += " ago";
    else output += " in the future";

    return output;
}

std::string getParentEvent(const tao::json::value &json) {
    std::string parent;

    const auto &tags = json.at("tags").get_array();

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

    return parent;
}

struct User {
    std::string pubkey;

    std::string username;
    tao::json::value kind0Json;

    User(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : pubkey(pubkey) {
        std::string prefix = pubkey;
        prefix += lmdb::to_sv<uint64_t>(0);

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == 0) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));

                try {
                    kind0Json = tao::json::from_string(json.at("content").get_string());
                    username = kind0Json.at("name").get_string();
                } catch (std::exception &e) {
                }
            }

            return false;
        });

        if (username.size() == 0) username = to_hex(pubkey.substr(0,4));
    }
};

struct UserCache {
    std::unordered_map<std::string, User> userCache;

    const User *getUser(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        auto u = userCache.find(pubkey);
        if (u != userCache.end()) return &u->second;

        userCache.emplace(pubkey, User(txn, decomp, pubkey));
        return &userCache.at(pubkey);
    }
};

struct EventThread {
    struct Event {
        defaultDb::environment::View_Event ev;
        tao::json::value json;

        flat_hash_set<std::string> children;
        uint64_t upVotes = 0;
        uint64_t downVotes = 0;
    };

    std::string id;
    bool found = false;
    flat_hash_map<std::string, Event> eventCache;
    UserCache userCache;


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
            std::string parent = getParentEvent(e.json);

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

        auto now = hoytech::curr_time_s();

        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            auto p = eventCache.find(id);
            if (p == eventCache.end()) throw herr("unknown id");
            const auto &elem = p->second;

            struct {
                std::string comment;
                std::string timestamp;
                const Event *ev;
                const User *user;
                std::vector<TemplarResult> replies;
            } ctx;

            ctx.comment = elem.json.at("content").get_string();
            ctx.timestamp = renderTimestamp(now, elem.json.at("created_at").get_unsigned());
            ctx.user = userCache.getUser(txn, decomp, std::string(sv(elem.ev.flat_nested()->pubkey())));
            ctx.ev = &elem;

            for (const auto &childId : elem.children) {
                ctx.replies.emplace_back(process(childId));
            }

            return tmpl::comment(ctx);
        };

        return process(id);
    }
};


struct UserComments {
    User u;

    struct Comment {
        tao::json::value json;
        std::string parent;
    };

    std::vector<Comment> comments;


    UserComments(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : u(txn, decomp, pubkey) {
        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s != pubkey || parsedKey.n1 != 1) return false;

            auto levId = lmdb::from_sv<uint64_t>(v);
            tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));
            std::string parent = getParentEvent(json);

            comments.emplace_back(Comment{ std::move(json), std::move(parent) });

            return true;
        }, true);
    }

    TemplarResult render(lmdb::txn &txn, Decompressor &decomp) {
        return tmpl::userComments(this);
    }
};



void WebServer::reply(const MsgReader::Request *msg, std::string_view r, std::string_view status) {
    std::string payload = "HTTP/1.0 ";
    payload += status;
    payload += "\r\nContent-Length: ";
    payload += std::to_string(r.size());
    payload += "\r\nContent-Type: text/html; charset=utf-8\r\n\r\n";
    payload += r;

    tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{msg->connId, msg->res, std::move(payload)}});
    hubTrigger->send();
}


void WebServer::handleRequest(lmdb::txn &txn, Decompressor &decomp, const MsgReader::Request *msg) {
    LI << "GOT REQUEST FOR " << msg->url;
    std::string fakeUrl = "http://localhost";
    fakeUrl += msg->url;
    uri u(fakeUrl);

    TemplarResult body;
    std::string_view code = "200 OK";

    if (u.get_path().starts_with("e/")) {
        auto eventId = from_hex(u.get_path().substr(2));
        EventThread et(txn, decomp, eventId);
        body = et.render(txn, decomp);
    } else if (u.get_path().starts_with("u/")) {
        auto pubkey = from_hex(u.get_path().substr(2));
        UserComments uc(txn, decomp, pubkey);
        body = uc.render(txn, decomp);
    } else {
        body = TemplarResult{ "Not found" };
        code = "404 Not Found";
    }


    std::string html;

    {
        struct {
            TemplarResult body;
        } ctx = {
            body,
        };

        html = tmpl::main(ctx).str;
    }

    reply(msg, html, code);
}



void WebServer::runReader(ThreadPool<MsgReader>::Thread &thr) {
    Decompressor decomp;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReader::Request>(&newMsg.msg)) {
                try {
                    handleRequest(txn, decomp, msg);
                } catch (std::exception &e) {
                    reply(msg, "Server error", "500 Server Error");
                    LE << "500 server error: " << e.what();
                }
            }
        }
    }
}
