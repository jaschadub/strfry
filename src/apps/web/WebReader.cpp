#include "WebServer.h"

#include "Bech32Utils.h"
#include "WebRenderUtils.h"
#include "WebTemplates.h"
#include "DBQuery.h"




struct User {
    std::string pubkey;

    std::string npubId;
    std::string username;
    tao::json::value kind0Json = tao::json::null;

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
        npubId = encodeBech32Simple("npub", pubkey);
    }

    bool kind0Found() const {
        return kind0Json.is_object();
    }

    std::string getMeta(std::string_view field) const {
        if (kind0Json.get_object().contains(field) && kind0Json.at(field).is_string()) return kind0Json.at(field).get_string();
        return "";
    }
};

struct UserCache {
    std::unordered_map<std::string, User> cache;

    const User *getUser(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        auto u = cache.find(pubkey);
        if (u != cache.end()) return &u->second;

        cache.emplace(pubkey, User(txn, decomp, pubkey));
        return &cache.at(pubkey);
    }
};


struct Event {
    defaultDb::environment::View_Event ev;

    tao::json::value json = tao::json::null;
    std::string parent;
    std::string root;

    flat_hash_set<std::string> children;
    uint64_t upVotes = 0;
    uint64_t downVotes = 0;


    Event(defaultDb::environment::View_Event ev) : ev(ev) {
    }

    static Event fromLevId(lmdb::txn &txn, uint64_t levId) {
        return Event(lookupEventByLevId(txn, levId));
    }

    static Event fromId(lmdb::txn &txn, std::string_view id) {
        auto existing = lookupEventById(txn, id);
        if (!existing) throw herr("unable to find event");
        return Event(std::move(*existing));
    }

    static Event fromIdExternal(lmdb::txn &txn, std::string_view id) {
        if (id.starts_with("note1")) {
            return fromId(txn, decodeBech32Simple(id));
        } else {
            return fromId(txn, from_hex(id));
        }
    }


    std::string getId() const {
        return std::string(sv(ev.flat_nested()->id()));
    }

    uint64_t getKind() const {
        return ev.flat_nested()->kind();
    }

    std::string getPubkey() const {
        return std::string(sv(ev.flat_nested()->pubkey()));
    }

    std::string getNoteId() const {
        return encodeBech32Simple("note", getId());
    }

    std::string getParentNoteId() const {
        return encodeBech32Simple("note", parent);
    }

    std::string getRootNoteId() const {
        return encodeBech32Simple("note", root);
    }

    std::string summary() const {
        // FIXME: Use "subject" tag if present?
        // FIXME: Don't truncate UTF-8 mid-sequence

        const size_t maxLen = 100;
        const auto &content = json.at("content").get_string();
        if (content.size() <= maxLen) return content;
        return content.substr(0, maxLen-3) + "...";
    }


    void populateJson(lmdb::txn &txn, Decompressor &decomp) {
        if (!json.is_null()) return;

        json = tao::json::from_string(getEventJson(txn, decomp, ev.primaryKeyId));
    }

    void populateRootParent(lmdb::txn &txn, Decompressor &decomp) {
        populateJson(txn, decomp);

        const auto &tags = json.at("tags").get_array();

        // Try to find a e-tags with root/reply types
        for (const auto &t : tags) {
            const auto &tArr = t.get_array();
            if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "root") {
                root = from_hex(tArr.at(1).get_string());
            } else if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "reply") {
                parent = from_hex(tArr.at(1).get_string());
            }
        }

        if (!root.size()) {
            // Otherwise, assume first e tag is root

            for (auto it = tags.begin(); it != tags.end(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    root = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }

        if (!parent.size()) {
            // Otherwise, assume last e tag is root

            for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    parent = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }
    }
};




struct EventThread {
    std::string rootEventId;
    bool isRootEventThreadRoot;
    flat_hash_map<std::string, Event> eventCache;
    UserCache userCache;


    EventThread(lmdb::txn &txn, Decompressor &decomp, std::string_view id_) : rootEventId(std::string(id_)) {
        try {
            eventCache.emplace(rootEventId, Event::fromId(txn, rootEventId));
        } catch (std::exception &e) {
            return;
        }


        eventCache.at(rootEventId).populateRootParent(txn, decomp);
        isRootEventThreadRoot = eventCache.at(rootEventId).root.empty();


        std::vector<std::string> pendingQueue;
        pendingQueue.emplace_back(rootEventId);

        while (pendingQueue.size()) {
            auto currId = std::move(pendingQueue.back());
            pendingQueue.pop_back();

            std::string prefix = "e";
            prefix += currId;

            env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
                ParsedKey_StringUint64 parsedKey(k);
                if (parsedKey.s != prefix) return false;

                auto levId = lmdb::from_sv<uint64_t>(v);
                Event e = Event::fromLevId(txn, levId);
                std::string childEventId = e.getId();

                if (eventCache.contains(childEventId)) return true;

                eventCache.emplace(childEventId, std::move(e));
                if (!isRootEventThreadRoot) pendingQueue.emplace_back(childEventId);

                return true;
            });
        }

        for (auto &[id, e] : eventCache) {
            e.populateRootParent(txn, decomp);

            if (e.parent.size()) {
                auto p = eventCache.find(e.parent);
                if (p != eventCache.end()) {
                    auto &parent = p->second;

                    auto kind = e.getKind();

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
        auto now = hoytech::curr_time_s();
        flat_hash_set<uint64_t> processedLevIds;

        struct RenderedEvent {
            std::string content;
            std::string timestamp;
            const Event *ev = nullptr;
            const User *user = nullptr;
            std::vector<TemplarResult> replies;
        };

        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            auto p = eventCache.find(id);
            if (p == eventCache.end()) throw herr("unknown id");
            const auto &elem = p->second;
            processedLevIds.insert(elem.ev.primaryKeyId);

            RenderedEvent ctx;

            ctx.content = elem.json.at("content").get_string();
            ctx.timestamp = renderTimestamp(now, elem.json.at("created_at").get_unsigned());
            ctx.user = userCache.getUser(txn, decomp, elem.getPubkey());
            ctx.ev = &elem;

            for (const auto &childId : elem.children) {
                ctx.replies.emplace_back(process(childId));
            }

            return tmpl::event::event(ctx);
        };




        struct {
            std::optional<RenderedEvent> threadRoot;
            TemplarResult foundEvents;
            std::vector<TemplarResult> orphanNodes;
        } ctx;

        std::optional<Event> summaryRootEvent;

        if (!isRootEventThreadRoot) {
            summaryRootEvent = Event::fromId(txn, eventCache.at(rootEventId).root);
            summaryRootEvent->populateJson(txn, decomp);

            ctx.threadRoot = RenderedEvent();
            auto &tr = *ctx.threadRoot;

            tr.content = summaryRootEvent->summary();
            tr.timestamp = renderTimestamp(now, summaryRootEvent->json.at("created_at").get_unsigned());
            tr.ev = &*summaryRootEvent;
            tr.user = userCache.getUser(txn, decomp, summaryRootEvent->getPubkey());
        }

        ctx.foundEvents = process(rootEventId);

        for (auto &[id, e] : eventCache) {
            if (processedLevIds.contains(e.ev.primaryKeyId)) continue;
            if (e.getKind() != 1) continue;

            ctx.orphanNodes.emplace_back(process(id));
        }

        return tmpl::events(ctx);
    }
};


struct UserComments {
    User u;

    struct Comment {
        tao::json::value json;
        std::string parent;
        std::string root;
    };

    std::vector<Comment> comments;


    UserComments(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : u(txn, decomp, pubkey) {
        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s != pubkey || parsedKey.n1 != 1) return false;

            auto levId = lmdb::from_sv<uint64_t>(v);
            tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));
            std::string parent = ""; // getParentEvent(json);
            std::string root = ""; // getRootEvent(json);

            comments.emplace_back(Comment{ std::move(json), std::move(parent), std::move(root) });

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


struct Url {
    std::vector<std::string_view> path;
    std::string_view query;

    Url(std::string_view u) {
        size_t pos;

        if ((pos = u.find("?")) != std::string::npos) {
            query = u.substr(pos + 1);
            u = u.substr(0, pos);
        }

        while ((pos = u.find("/")) != std::string::npos) {
            if (pos != 0) path.emplace_back(u.substr(0, pos));
            u = u.substr(pos + 1);
        }

        if (u.size()) path.emplace_back(u);
    }
};

void WebServer::handleRequest(lmdb::txn &txn, Decompressor &decomp, const MsgReader::Request *msg) {
    LI << "GOT REQUEST FOR " << msg->url;
    auto startTime = hoytech::curr_time_us();

    Url u(msg->url);

    TemplarResult body;
    std::string_view code = "200 OK";

    if (u.path.size() == 0) {
        body = TemplarResult{ "root" };
    } else if (u.path[0] == "e" && u.path.size() == 2) {
        EventThread et(txn, decomp, decodeBech32Simple(u.path[1]));
        body = et.render(txn, decomp);
    } else if (u.path[0] == "u" && u.path.size() == 2) {
        User user(txn, decomp, decodeBech32Simple(u.path[1]));
        body = tmpl::user(user);
    } else if (u.path[0] == "u" && u.path.size() == 3 && u.path[2] == "notes") {
        UserComments uc(txn, decomp, decodeBech32Simple(u.path[1]));
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

    LI << "Reply: " << code << " / " << html.size() << " bytes in " << (hoytech::curr_time_us() - startTime) << "us";
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
