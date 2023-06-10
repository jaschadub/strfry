#include "WebServer.h"

#include "DBQuery.h"
#include "WebTemplates.h"


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


                std::string rootIdHex = "71f42777f35e4636dce9283b88ba9367f66e99b52e1123dc619ef6fcd9567df3";

                tao::json::value events = tao::json::empty_array;

                tao::json::value filter = tao::json::value::array({
                    { { "ids", tao::json::value::array({ rootIdHex }) } },
                    { { "#e", tao::json::value::array({ rootIdHex }) } },
                });


                struct Elem {
                    tao::json::value ev;
                    flat_hash_set<std::string> children;
                };

                flat_hash_map<std::string, Elem> idToElem;


                foreachByFilter(txn, filter, [&](uint64_t levId, std::string_view evJson){
                    tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId, evJson));
                    std::string id = from_hex(json.at("id").get_string());

                    idToElem.emplace(id, Elem{ std::move(json) });
                });


                for (const auto &[id, e] : idToElem) {
                    std::string parent;

                    const auto &tags = e.ev.at("tags").get_array();

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
                        auto p = idToElem.find(parent);
                        if (p != idToElem.end()) {
                            p->second.children.insert(id);
                        }
                    }
                }


                std::function<::tmplInternal::Result(const std::string &)> process = [&](const std::string &id){
                    auto p = idToElem.find(id);
                    if (p == idToElem.end()) throw herr("unknown id");
                    const auto &elem = p->second;

                    struct {
                        std::string comment;
                        std::vector<::tmplInternal::Result> replies;
                    } ctx;

                    ctx.comment = elem.ev.at("content").get_string();

                    for (const auto &childId : elem.children) {
                        ctx.replies.emplace_back(process(childId));
                    }

                    return ::tmpl::comment(ctx);
                };


                auto body = process(from_hex(rootIdHex));

                std::string html;

                {
                    struct {
                        ::tmplInternal::Result body;
                    } ctx = {
                        body,
                    };

                    html = ::tmpl::main(ctx).str;
                }

                respond(html);
            }
        }
    }
}
