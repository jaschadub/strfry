#pragma once

#include <iostream>
#include <memory>
#include <algorithm>

#include <hoytech/time.h>
#include <hoytech/hex.h>
#include <hoytech/file_change_monitor.h>
#include <uWebSockets/src/uWS.h>
#include <tao/json.hpp>

#include "golpe.h"

#include "HTTPResponder.h"
#include "ThreadPool.h"
#include "Decompressor.h"



struct Connection : NonCopyable {
    uWS::HttpSocket<uWS::SERVER> *httpsocket;
    uint64_t connId;
    uint64_t connectedTimestamp;
    flat_hash_set<uWS::HttpResponse *> pendingRequests;

    Connection(uWS::HttpSocket<uWS::SERVER> *hs, uint64_t connId_)
        : httpsocket(hs), connId(connId_), connectedTimestamp(hoytech::curr_time_us()) { }
    Connection(const Connection &) = delete;
    Connection(Connection &&) = delete;
};




struct MsgHttpsocket : NonCopyable {
    struct Send {
        uint64_t connId;
        uWS::HttpResponse *res;
        std::string payload;
        uint64_t lockedThreadId;
    };

    using Var = std::variant<Send>;
    Var msg;
    MsgHttpsocket(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgWebReader : NonCopyable {
    struct Request {
        HTTPReq req;
        uint64_t lockedThreadId;
    };

    using Var = std::variant<Request>;
    Var msg;
    MsgWebReader(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgWebWriter : NonCopyable {
    struct Request {
        HTTPReq req;
    };

    using Var = std::variant<Request>;
    Var msg;
    MsgWebWriter(Var &&msg_) : msg(std::move(msg_)) {}
};


struct WebServer {
    std::unique_ptr<uS::Async> hubTrigger;
    HTTPResponder httpResponder;

    // Thread Pools

    ThreadPool<MsgHttpsocket> tpHttpsocket;
    ThreadPool<MsgWebReader> tpReader;
    ThreadPool<MsgWebWriter> tpWriter;

    void run();

    void runHttpsocket(ThreadPool<MsgHttpsocket>::Thread &thr);
    void dispatchPostRequest();

    void runReader(ThreadPool<MsgWebReader>::Thread &thr);
    void handleReadRequest(lmdb::txn &txn, Decompressor &decomp, const MsgWebReader::Request *msg);

    void runWriter(ThreadPool<MsgWebWriter>::Thread &thr);

    // Utils

    // Moves from payload!
    void sendHttpResponseAndUnlock(uint64_t lockedThreadId, const HTTPReq &req, std::string &payload) {
        tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{req.connId, req.res, std::move(payload), lockedThreadId}});
        hubTrigger->send();
    }

    void sendHttpResponse(const HTTPReq &req, std::string_view body, std::string_view code = "200 OK", std::string_view contentType = "text/html; charset=utf-8") {
        HTTPResponseData res;
        res.code = code;
        res.contentType = contentType;
        res.body = std::string(body); // FIXME: copy

        std::string payload = res.encode(false);

        sendHttpResponseAndUnlock(MAX_U64, req, payload);
    }
};
