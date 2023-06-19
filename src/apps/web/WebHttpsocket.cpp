#include "WebServer.h"

#include "app_git_version.h"



void WebServer::runHttpsocket(ThreadPool<MsgHttpsocket>::Thread &thr) {
    struct Connection {
        uWS::HttpSocket<uWS::SERVER> *httpsocket;
        uint64_t connId;
        uint64_t connectedTimestamp;
        flat_hash_set<uWS::HttpResponse *> pendingRequests;

        Connection(uWS::HttpSocket<uWS::SERVER> *hs, uint64_t connId_)
            : httpsocket(hs), connId(connId_), connectedTimestamp(hoytech::curr_time_us()) { }
        Connection(const Connection &) = delete;
        Connection(Connection &&) = delete;
    };

    uWS::Hub hub;
    uWS::Group<uWS::SERVER> *hubGroup;
    flat_hash_map<uint64_t, Connection*> connIdToConnection;
    uint64_t nextConnectionId = 1;

    {
        int extensionOptions = 0;

        hubGroup = hub.createGroup<uWS::SERVER>(extensionOptions);
    }


    hubGroup->onHttpConnection([&](uWS::HttpSocket<uWS::SERVER> *hs) {
        uint64_t connId = nextConnectionId++;
        Connection *c = new Connection(hs, connId);

        hs->setUserData((void*)c);
        connIdToConnection.emplace(connId, c);
    });

    hubGroup->onHttpDisconnection([&](uWS::HttpSocket<uWS::SERVER> *hs) {
        auto *c = (Connection*)hs->getUserData();

        connIdToConnection.erase(c->connId);
        delete c;
    });

    hubGroup->onHttpRequest([&](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t length, size_t remainingBytes){
        auto *c = (Connection*)res->httpSocket->getUserData();
        c->pendingRequests.insert(res);

        bool acceptGzip = req.getHeader("accept-encoding").toStringView().find("gzip") != std::string::npos;

        res->hasHead = true;
        tpReader.dispatch(c->connId, MsgReader{MsgReader::Request{c->connId, res, req.getUrl().toString(), acceptGzip}});
    });



    std::function<void()> asyncCb = [&]{
        auto newMsgs = thr.inbox.pop_all_no_wait();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgHttpsocket::Send>(&newMsg.msg)) {
                auto it = connIdToConnection.find(msg->connId);
                if (it == connIdToConnection.end()) continue;
                auto &c = *it->second;

                if (!c.pendingRequests.contains(msg->res)) {
                    LW << "Couldn't find request in pendingRequests set";
                    continue;
                }

                c.pendingRequests.erase(msg->res);

                msg->res->end(msg->payload.data(), msg->payload.size());
            }
        }
    };

    hubTrigger = std::make_unique<uS::Async>(hub.getLoop());
    hubTrigger->setData(&asyncCb);

    hubTrigger->start([](uS::Async *a){
        auto *r = static_cast<std::function<void()> *>(a->data);
        (*r)();
    });



    int port = cfg().web__port;

    std::string bindHost = cfg().web__bind;

    if (!hub.listen(bindHost.c_str(), port, nullptr, uS::REUSE_PORT, hubGroup)) throw herr("unable to listen on port ", port);

    LI << "Started http server on " << bindHost << ":" << port;

    hub.run();
}
