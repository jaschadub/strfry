#include "WebServer.h"

#include "WebUtils.h"





void WebServer::handleWriteRequest(lmdb::txn &txn, Decompressor &decomp, const MsgWriter::Request *msg) {
    auto startTime = hoytech::curr_time_us();
    const auto &req = msg->req;
    Url u(req.url);

    LI << "WRITE REQUEST: " << req.url;

    std::string_view code = "200 OK";
    std::string_view contentType = "application/json; charset=utf-8";
    std::optional<tao::json::value> body;

    if (u.path.size() == 1) {
        if (u.path[0] == "submit-post") {
            LI << "NEW POST: " << req.body;
        }
    }

    std::string responseData;

    if (body) {
        responseData = tao::json::to_string(*body);
    } else {
        code = "404 Not Found";
        responseData = tao::json::to_string(tao::json::value({{ "err", "not found" }}));
    }

    LI << "Reply: " << code << " / " << responseData.size() << " bytes in " << (hoytech::curr_time_us() - startTime) << "us";
    sendHttpResponse(req, responseData, code, contentType);
}



void WebServer::runWriter(ThreadPool<MsgWriter>::Thread &thr) {
    Decompressor decomp;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWriter::Request>(&newMsg.msg)) {
                try {
                    handleWriteRequest(txn, decomp, msg);
                } catch (std::exception &e) {
                    sendHttpResponse(msg->req, "Server error", "500 Server Error");
                    LE << "500 server error: " << e.what();
                }
            }
        }
    }
}
