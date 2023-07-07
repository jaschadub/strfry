#include "zlib.h"

#include "WebServer.h"


void WebServer::sendHttpResponseAndUnlock(uint64_t lockedThreadId, const HTTPReq &req, std::string_view body, std::string_view status, std::string_view contentType) {
    bool didCompress = false;
    std::string compressed;

    if (body.size() > 512 && req.acceptGzip) {
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

    payload += "HTTP/1.1 ";
    payload += status;
    payload += "\r\nContent-Length: ";
    payload += std::to_string(body.size());
    payload += "\r\nContent-Type: ";
    payload += contentType;
    payload += "\r\n";
    if (didCompress) payload += "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n";
    payload += "Connection: Keep-Alive\r\n\r\n";
    payload += body;

    tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{req.connId, req.res, std::move(payload), lockedThreadId}});
    hubTrigger->send();
}
