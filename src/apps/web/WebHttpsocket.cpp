#include <queue>

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


    std::vector<bool> tpReaderLock(tpReader.numThreads, false);
    std::queue<MsgReader> pendingReaderMessages;


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
        res->hasHead = true;

        bool acceptGzip = req.getHeader("accept-encoding").toStringView().find("gzip") != std::string::npos;

        auto m = MsgReader{MsgReader::Request{MAX_U64, c->connId, res, req.getUrl().toString(), acceptGzip}};
        bool didDispatch = false;

        for (uint64_t i = 0; i < tpReader.numThreads; i++) {
            if (tpReaderLock[i] == false) {
                tpReaderLock[i] = true;
                std::get<MsgReader::Request>(m.msg).lockedThreadId = i;
                tpReader.dispatch(i, std::move(m));
                didDispatch = true;
                break;
            }
        }

        if (!didDispatch) pendingReaderMessages.emplace(std::move(m));
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

                if (msg->lockedThreadId != MAX_U64) {
                    if (tpReaderLock.at(msg->lockedThreadId) == false) throw herr("tried to unlock already unlocked reader lock!");

                    if (pendingReaderMessages.empty()) {
                        tpReaderLock[msg->lockedThreadId] = false;
                    } else {
                        std::get<MsgReader::Request>(pendingReaderMessages.front().msg).lockedThreadId = msg->lockedThreadId;
                        tpReader.dispatch(msg->lockedThreadId, std::move(pendingReaderMessages.front()));
                        pendingReaderMessages.pop();
                    }
                }
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
