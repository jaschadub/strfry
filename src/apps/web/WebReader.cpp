#include "zlib.h"
#include "re2/re2.h"

#include "WebServer.h"

#include "Bech32Utils.h"
#include "WebRenderUtils.h"
#include "WebTemplates.h"
#include "DBQuery.h"






struct User {
    std::string pubkey;

    std::string npubId;
    std::string username;
    std::optional<tao::json::value> kind0Json;
    std::optional<tao::json::value> kind3Event;

    User(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : pubkey(pubkey) {
        npubId = encodeBech32Simple("npub", pubkey);

        kind0Json = loadKindJson(txn, decomp, 0);

        try {
            if (kind0Json) username = kind0Json->at("name").get_string();
        } catch (std::exception &e) {
        }

        if (username.size() == 0) username = to_hex(pubkey.substr(0,4));
    }

    std::optional<tao::json::value> loadKindJson(lmdb::txn &txn, Decompressor &decomp, uint64_t kind) {
        std::optional<tao::json::value> output;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));

                try {
                    output = tao::json::from_string(json.at("content").get_string());
                    if (!output->is_object()) output = std::nullopt;
                } catch (std::exception &e) {
                }
            }

            return false;
        });

        return output;
    }

    std::optional<tao::json::value> loadKindEvent(lmdb::txn &txn, Decompressor &decomp, uint64_t kind) {
        std::optional<tao::json::value> output;

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, kind, 0), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == kind) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                output = tao::json::from_string(getEventJson(txn, decomp, levId));
            }

            return false;
        });

        return output;
    }

    bool kind0Found() const {
        return !!kind0Json;
    }

    std::string getMeta(std::string_view field) const {
        if (!kind0Json) throw herr("can't getMeta because user doesn't have kind 0");
        if (kind0Json->get_object().contains(field) && kind0Json->at(field).is_string()) return kind0Json->at(field).get_string();
        return "";
    }

    void populateContactList(lmdb::txn &txn, Decompressor &decomp) {
        kind3Event = loadKindEvent(txn, decomp, 3);
    }

    std::vector<std::string> getFollowers(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        std::vector<std::string> output;
        flat_hash_set<std::string> alreadySeen;

        std::string prefix = "p";
        prefix += pubkey;

        env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64 parsedKey(k);
            if (parsedKey.s != prefix) return false;

            auto levId = lmdb::from_sv<uint64_t>(v);
            auto ev = lookupEventByLevId(txn, levId);

            if (ev.flat_nested()->kind() == 3) {
                auto pubkey = std::string(sv(ev.flat_nested()->pubkey()));

                if (!alreadySeen.contains(pubkey)) {
                    alreadySeen.insert(pubkey);
                    output.emplace_back(std::move(pubkey));
                }
            }

            return true;
        });

        return output;
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

    uint64_t getCreatedAt() const {
        return ev.flat_nested()->created_at();
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
        // FIXME: Don't put ellipsis if truncated text ends in punctuation

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




std::string preprocessContent(lmdb::txn &txn, Decompressor &decomp, const Event &ev, UserCache &userCache, std::string_view content) {
    static RE2 matcher(R"((?is)(.*?)(https?://\S+|#\[\d+\]))");

    std::string output;

    re2::StringPiece input(content);
    re2::StringPiece prefix, match;

    auto sv = [](re2::StringPiece s){ return std::string_view(s.data(), s.size()); };
    auto appendLink = [&](std::string_view url, std::string_view text){
        output += "<a href=\"";
        output += url;
        output += "\">";
        output += text;
        output += "</a>";
    };

    while (RE2::Consume(&input, matcher, &prefix, &match)) {
        output += sv(prefix);

        if (match.starts_with("http")) {
            appendLink(sv(match), sv(match));
        } else if (match.starts_with("#[")) {
            bool didTransform = false;
            auto offset = std::stoull(std::string(sv(match)).substr(2, match.size() - 3));

            const auto &tags = ev.json.at("tags").get_array();

            try {
                const auto &tag = tags.at(offset).get_array();

                if (tag.at(0) == "p") {
                    const auto *u = userCache.getUser(txn, decomp, from_hex(tag.at(1).get_string()));
                    appendLink(std::string("/u/") + u->npubId, u->username);
                    didTransform = true;
                } else if (tag.at(0) == "e") {
                    appendLink(std::string("/e/") + encodeBech32Simple("note", from_hex(tag.at(1).get_string())), sv(match));
                    didTransform = true;
                }
            } catch(std::exception &e) {
                //LW << "tag parse error: " << e.what();
            }

            if (!didTransform) output += sv(match);
        }
    }

    output += std::string_view(input.data(), input.size());

    return output;
}



struct EventThread {
    std::string rootEventId;
    bool isRootEventThreadRoot;
    flat_hash_map<std::string, Event> eventCache;

    flat_hash_map<std::string, flat_hash_set<std::string>> children; // parentEventId -> childEventIds


    // Load all events under an eventId

    EventThread(std::string rootEventId, bool isRootEventThreadRoot, flat_hash_map<std::string, Event> &&eventCache)
        : rootEventId(rootEventId), isRootEventThreadRoot(isRootEventThreadRoot), eventCache(eventCache) {}

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

            auto kind = e.getKind();

            if (e.parent.size()) {
                if (kind == 1) {
                    if (!children.contains(e.parent)) children.emplace(std::piecewise_construct, std::make_tuple(e.parent), std::make_tuple());
                    children.at(e.parent).insert(id);
                } else if (kind == 7) {
                    auto p = eventCache.find(e.parent);
                    if (p != eventCache.end()) {
                        auto &parent = p->second;

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


    TemplarResult render(lmdb::txn &txn, Decompressor &decomp, UserCache &userCache, std::optional<std::string> focusOnPubkey = std::nullopt) {
        auto now = hoytech::curr_time_s();
        flat_hash_set<uint64_t> processedLevIds;

        struct Reply {
            uint64_t timestamp;
            TemplarResult rendered;
        };

        struct RenderedEvent {
            std::string content;
            std::string timestamp;
            const Event *ev = nullptr;
            const User *user = nullptr;
            bool eventPresent = true;
            bool abbrev = false;
            std::vector<Reply> replies;
        };

        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            RenderedEvent ctx;

            auto p = eventCache.find(id);
            if (p != eventCache.end()) {
                const auto &elem = p->second;
                processedLevIds.insert(elem.ev.primaryKeyId);

                auto pubkey = elem.getPubkey();
                ctx.abbrev = focusOnPubkey && *focusOnPubkey != pubkey;

                ctx.content = ctx.abbrev ? elem.summary() : elem.json.at("content").get_string();
                ctx.timestamp = renderTimestamp(now, elem.getCreatedAt());
                ctx.user = userCache.getUser(txn, decomp, elem.getPubkey());
                ctx.eventPresent = true;

                ctx.ev = &elem;

                ctx.content = templarInternal::htmlEscape(ctx.content, false);
                ctx.content = preprocessContent(txn, decomp, elem, userCache, ctx.content);
            } else {
                ctx.eventPresent = false;
            }

            if (children.contains(id)) {
                for (const auto &childId : children.at(id)) {
                    auto timestamp = MAX_U64;
                    auto p = eventCache.find(childId);
                    if (p != eventCache.end()) timestamp = p->second.getCreatedAt();

                    ctx.replies.emplace_back(timestamp, process(childId));
                }

                std::sort(ctx.replies.begin(), ctx.replies.end(), [](auto &a, auto &b){ return a.timestamp < b.timestamp; });
            }

            return tmpl::event::event(ctx);
        };


        struct {
            TemplarResult foundEvents;
            std::vector<Reply> orphanNodes;
        } ctx;

        ctx.foundEvents = process(rootEventId);

        for (auto &[id, e] : eventCache) {
            if (processedLevIds.contains(e.ev.primaryKeyId)) continue;
            if (e.getKind() != 1) continue;

            ctx.orphanNodes.emplace_back(e.getCreatedAt(), process(id));
        }

        std::sort(ctx.orphanNodes.begin(), ctx.orphanNodes.end(), [](auto &a, auto &b){ return a.timestamp < b.timestamp; });

        return tmpl::events(ctx);
    }
};



struct UserEvents {
    User u;

    struct EventCluster {
        std::string rootEventId;
        flat_hash_map<std::string, Event> eventCache; // eventId (non-root) -> Event
        bool isRootEventFromUser = false;
        bool isRootPresent = false;
        uint64_t rootEventTimestamp = 0;

        EventCluster(std::string rootEventId) : rootEventId(rootEventId) {}
    };

    std::vector<EventCluster> eventClusterArr;

    UserEvents(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : u(txn, decomp, pubkey) {
        flat_hash_map<std::string, EventCluster> eventClusters; // eventId (root) -> EventCluster

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);
            if (parsedKey.s != pubkey || parsedKey.n1 != 1) return false;

            Event ev = Event::fromLevId(txn, lmdb::from_sv<uint64_t>(v));
            ev.populateRootParent(txn, decomp);
            auto id = ev.getId();

            auto installRoot = [&](std::string rootId, Event &&rootEvent){
                rootEvent.populateRootParent(txn, decomp);

                eventClusters.emplace(rootId, rootId);
                auto &cluster = eventClusters.at(rootId);

                cluster.isRootPresent = true;
                cluster.isRootEventFromUser = rootEvent.getPubkey() == u.pubkey;
                cluster.rootEventTimestamp = rootEvent.getCreatedAt();
                cluster.eventCache.emplace(rootId, std::move(rootEvent));
            };

            if (ev.root.size()) {
                // Event is not root

                if (!eventClusters.contains(ev.root)) {
                    try {
                        installRoot(ev.root, Event::fromId(txn, ev.root));
                    } catch (std::exception &e) {
                        // no root event
                        eventClusters.emplace(ev.root, ev.root);
                        auto &cluster = eventClusters.at(ev.root);

                        cluster.isRootPresent = true;
                    }
                }

                eventClusters.at(ev.root).eventCache.emplace(id, std::move(ev));
            } else {
                // Event is root

                if (!eventClusters.contains(ev.root)) {
                    installRoot(id, std::move(ev));
                }
            }

            return true;
        }, true);

        for (auto &[k, v] : eventClusters) {
            eventClusterArr.emplace_back(std::move(v));
        }

        std::sort(eventClusterArr.begin(), eventClusterArr.end(), [](auto &a, auto &b){ return b.rootEventTimestamp < a.rootEventTimestamp; });
    }

    TemplarResult render(lmdb::txn &txn, Decompressor &decomp) {
        std::vector<TemplarResult> renderedThreads;
        UserCache userCache;

        for (auto &cluster : eventClusterArr) {
            EventThread eventThread(cluster.rootEventId, cluster.isRootEventFromUser, std::move(cluster.eventCache));
            renderedThreads.emplace_back(eventThread.render(txn, decomp, userCache, u.pubkey));
        }

        struct {
            std::vector<TemplarResult> &renderedThreads;
            User &u;
        } ctx = {
            renderedThreads,
            u,
        };

        return tmpl::user::comments(ctx);
    }
};




std::string exportUserEvents(lmdb::txn &txn, Decompressor &decomp, std::string_view pubkey) {
    std::string output;

    env.generic_foreachFull(txn, env.dbi_Event__pubkey, makeKey_StringUint64(pubkey, MAX_U64), "", [&](std::string_view k, std::string_view v){
        ParsedKey_StringUint64 parsedKey(k);
        if (parsedKey.s != pubkey) return false;

        uint64_t levId = lmdb::from_sv<uint64_t>(v);
        output += getEventJson(txn, decomp, levId);
        output += "\n";

        return true;
    }, true);

    return output;
}








void WebServer::reply(const MsgReader::Request *msg, std::string_view body, std::string_view status, std::string_view contentType) {
    bool didCompress = false;
    std::string compressed;

    if (body.size() > 512 && msg->acceptGzip) {
        compressed.resize(body.size());

        z_stream zs;
        zs.zalloc = Z_NULL;
        zs.zfree = Z_NULL;
        zs.opaque = Z_NULL;
        zs.avail_in = body.size();
        zs.next_in = (Bytef*)body.data();
        zs.avail_out = compressed.size();
        zs.next_out = (Bytef*)compressed.data();

        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
        auto ret1 = deflate(&zs, Z_FINISH);
        auto ret2 = deflateEnd(&zs);

        if (ret1 == Z_STREAM_END && ret2 == Z_OK) {
            compressed.resize(zs.total_out);
            didCompress = true;
            body = compressed;
        } else {
            compressed = "";
        }
    }

    std::string payload;
    payload.reserve(body.size() + 1024);

    payload += "HTTP/1.0 ";
    payload += status;
    payload += "\r\nContent-Length: ";
    payload += std::to_string(body.size());
    payload += "\r\nContent-Type: ";
    payload += contentType;
    payload += "\r\n";
    if (didCompress) payload += "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n";
    payload += "Connection: Keep-Alive\r\n\r\n";
    payload += body;

    tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{msg->lockedThreadId, msg->connId, msg->res, std::move(payload)}});
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

    UserCache userCache;

    std::optional<std::string> rawBody;
    std::optional<TemplarResult> body;
    std::string_view code = "200 OK";
    std::string_view contentType = "text/html; charset=utf-8";

    if (u.path.size() == 0) {
        body = TemplarResult{ "root" };
    } else if (u.path[0] == "e") {
        if (u.path.size() == 2) {
            EventThread et(txn, decomp, decodeBech32Simple(u.path[1]));
            body = et.render(txn, decomp, userCache);
        }
    } else if (u.path[0] == "u") {
        if (u.path.size() == 2) {
            User user(txn, decomp, decodeBech32Simple(u.path[1]));
            body = tmpl::user::metadata(user);
        } else if (u.path.size() == 3) {
            if (u.path[2] == "notes") {
                UserEvents uc(txn, decomp, decodeBech32Simple(u.path[1]));
                body = uc.render(txn, decomp);
            } else if (u.path[2] == "export.jsonl") {
                rawBody = exportUserEvents(txn, decomp, decodeBech32Simple(u.path[1]));
                contentType = "application/json; charset=utf-8";
            } else if (u.path[2] == "following") {
                User user(txn, decomp, decodeBech32Simple(u.path[1]));
                user.populateContactList(txn, decomp);

                struct {
                    User &user;
                    std::function<const User*(const std::string &)> getUser;
                } ctx = {
                    user,
                    [&](const std::string &pubkey){ return userCache.getUser(txn, decomp, pubkey); },
                };

                body = tmpl::user::following(ctx);
            } else if (u.path[2] == "followers") {
                User user(txn, decomp, decodeBech32Simple(u.path[1]));
                auto followers = user.getFollowers(txn, decomp, user.pubkey);

                struct {
                    const User &user;
                    const std::vector<std::string> &followers;
                    std::function<const User*(const std::string &)> getUser;
                } ctx = {
                    user,
                    followers,
                    [&](const std::string &pubkey){ return userCache.getUser(txn, decomp, pubkey); },
                };

                body = tmpl::user::followers(ctx);
            }
        }
    }




    std::string responseData;

    if (body) {
        struct {
            TemplarResult body;
            std::string title;
            std::string staticFilesPrefix;
        } ctx = {
            *body,
            "",
            "http://127.0.0.1:8081",
        };

        responseData = std::move(tmpl::main(ctx).str);
    } else if (rawBody) {
        responseData = std::move(*rawBody);
    } else {
        body = TemplarResult{ "Not found" };
        code = "404 Not Found";
    }

    LI << "Reply: " << code << " / " << responseData.size() << " bytes in " << (hoytech::curr_time_us() - startTime) << "us";
    reply(msg, responseData, code, contentType);
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
