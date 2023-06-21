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

#include "ThreadPool.h"
#include "Decompressor.h"



struct HTTPReq : NonCopyable {
    uint64_t connId;
    uWS::HttpResponse *res;

    std::string url;
    uWS::HttpMethod method = uWS::HttpMethod::METHOD_INVALID;
    bool acceptGzip = false;

    std::string body;

    HTTPReq(uint64_t connId, uWS::HttpResponse *res, uWS::HttpRequest req) : connId(connId), res(res) {
        res->hasHead = true; // We'll be sending our own headers

        method = req.getMethod();
        url = req.getUrl().toString();
        acceptGzip = req.getHeader("accept-encoding").toStringView().find("gzip") != std::string::npos;
    }
};

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

struct MsgReader : NonCopyable {
    struct Request {
        HTTPReq req;
        uint64_t lockedThreadId;
    };

    using Var = std::variant<Request>;
    Var msg;
    MsgReader(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgWriter : NonCopyable {
    struct Request {
        HTTPReq req;
    };

    using Var = std::variant<Request>;
    Var msg;
    MsgWriter(Var &&msg_) : msg(std::move(msg_)) {}
};


struct WebServer {
    std::unique_ptr<uS::Async> hubTrigger;

    // Thread Pools

    ThreadPool<MsgHttpsocket> tpHttpsocket;
    ThreadPool<MsgReader> tpReader;
    ThreadPool<MsgWriter> tpWriter;

    void run();

    void runHttpsocket(ThreadPool<MsgHttpsocket>::Thread &thr);
    void dispatchPostRequest();

    void runReader(ThreadPool<MsgReader>::Thread &thr);
    void handleReadRequest(lmdb::txn &txn, Decompressor &decomp, const MsgReader::Request *msg);

    void runWriter(ThreadPool<MsgWriter>::Thread &thr);
    void handleWriteRequest(lmdb::txn &txn, Decompressor &decomp, const MsgWriter::Request *msg);

    // Utils

    void sendHttpResponseAndUnlock(uint64_t lockedThreadId, const HTTPReq &req, std::string_view body, std::string_view status = "200 OK", std::string_view contentType = "text/html; charset=utf-8");

    void sendHttpResponse(const HTTPReq &req, std::string_view body, std::string_view status = "200 OK", std::string_view contentType = "text/html; charset=utf-8") {
        sendHttpResponseAndUnlock(MAX_U64, req, body, status, contentType);
    }
};
