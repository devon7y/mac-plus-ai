// Plus Modem: an HTTP/1.1 response reader (status, headers, chunked or plain body) and an SSE parser,
// both fed byte by byte from the network so a reply is never held whole in memory.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

class HttpResponse {
public:
    HttpResponse() { reset(); }
    void reset();
    // Bytes from the connection; decoded body bytes are appended to body. False: not HTTP.
    bool feed(const uint8_t *p, size_t n, std::string &body);
    bool headersDone() const { return st_ >= H_BODY; }
    bool done() const { return st_ == H_DONE; }         // the whole body has arrived (chunked or Content-Length)
    int status;
private:
    enum { H_STATUS, H_HEADERS, H_BODY, H_CHUNK_SIZE, H_CHUNK_DATA, H_CHUNK_END, H_TRAILER, H_DONE, H_BAD };
    int st_;
    bool chunked_;
    long length_;                                      // -1 unknown (read until the connection closes)
    long remain_;
    std::string line_;
    bool header(const std::string &h);
};

struct SseEvent { std::string event, data; };

class SseParser {
public:
    SseParser() { reset(); }
    void reset();
    void feed(const char *p, size_t n, std::vector<SseEvent> &out);
    void finish(std::vector<SseEvent> &out);            // the stream ended: dispatch a final event
private:
    std::string line_, event_, data_;
    int dataLines_;
    bool overflow_;
    void handle(std::vector<SseEvent> &out);
};
