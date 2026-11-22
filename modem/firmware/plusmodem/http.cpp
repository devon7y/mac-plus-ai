#include "http.h"
#include <stdlib.h>
#include "config.h"

static const size_t MAX_HEADER = 2048;
static const size_t MAX_SSE_LINE = 16384;

void HttpResponse::reset()
{
    status = 0;
    st_ = H_STATUS;
    chunked_ = false;
    length_ = -1;
    remain_ = 0;
    line_.clear();
}

bool HttpResponse::header(const std::string &h)
{
    size_t c = h.find(':');
    if (c == std::string::npos) return true;
    std::string name = lower(trim(h.substr(0, c))), value = lower(trim(h.substr(c + 1)));
    if (name == "transfer-encoding" && value.find("chunked") != std::string::npos) chunked_ = true;
    if (name == "content-length") length_ = atol(value.c_str());
    return true;
}

bool HttpResponse::feed(const uint8_t *p, size_t n, std::string &body)
{
    size_t i = 0;
    while (i < n) {
        if (st_ == H_BAD) return false;
        if (st_ == H_DONE) return true;
        if (st_ == H_BODY) {                                     // plain body
            size_t k = n - i;
            if (length_ >= 0 && (long)k > remain_) k = (size_t)remain_;
            body.append((const char *)p + i, k);
            i += k;
            if (length_ >= 0 && (remain_ -= (long)k) == 0) st_ = H_DONE;
            continue;
        }
        if (st_ == H_CHUNK_DATA) {
            size_t k = n - i;
            if ((long)k > remain_) k = (size_t)remain_;
            body.append((const char *)p + i, k);
            i += k;
            if ((remain_ -= (long)k) == 0) st_ = H_CHUNK_END;
            continue;
        }
        char c = (char)p[i++];                                 // line-based states
        if (c != '\n') {
            if (c != '\r' && line_.size() < MAX_HEADER) line_ += c;
            continue;
        }
        std::string l;
        l.swap(line_);
        switch (st_) {
        case H_STATUS:
            if (l.compare(0, 5, "HTTP/") != 0) { st_ = H_BAD; return false; }
            status = atoi(l.c_str() + l.find(' ') + 1);
            st_ = H_HEADERS;
            break;
        case H_HEADERS:
            if (!l.empty()) { header(l); break; }
            if (status == 100) { st_ = H_STATUS; break; }        // "100 Continue": the real status follows
            if (chunked_) st_ = H_CHUNK_SIZE;
            else if (length_ == 0 || status == 204 || status == 304) st_ = H_DONE;
            else { st_ = H_BODY; remain_ = length_; }
            break;
        case H_CHUNK_SIZE:
            remain_ = strtol(l.c_str(), 0, 16);
            st_ = remain_ > 0 ? H_CHUNK_DATA : H_TRAILER;
            break;
        case H_CHUNK_END:                                        // the CRLF after a chunk's data
            st_ = H_CHUNK_SIZE;
            break;
        case H_TRAILER:
            if (l.empty()) st_ = H_DONE;
            break;
        }
    }
    return st_ != H_BAD;
}

void SseParser::reset()
{
    line_.clear();
    event_.clear();
    data_.clear();
    dataLines_ = 0;
    overflow_ = false;
}

void SseParser::handle(std::vector<SseEvent> &out)
{
    std::string l;
    l.swap(line_);
    if (overflow_) { overflow_ = false; return; }             // a line too long for us: dropped
    if (!l.empty() && l[l.size() - 1] == '\r') l.erase(l.size() - 1);
    if (l.empty()) {                                           // a blank line ends the event
        if (dataLines_) {
            SseEvent e;
            e.event = event_.empty() ? "message" : event_;
            e.data.swap(data_);
            out.push_back(e);
        }
        event_.clear();
        data_.clear();
        dataLines_ = 0;
        return;
    }
    if (l[0] == ':') return;                                   // a comment (keep-alive)
    size_t c = l.find(':');
    std::string field = l.substr(0, c), value = c == std::string::npos ? "" : l.substr(c + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    if (field == "event") event_ = value;
    else if (field == "data") {
        if (dataLines_++) data_ += '\n';
        data_ += value;
    }
}

void SseParser::feed(const char *p, size_t n, std::vector<SseEvent> &out)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') { handle(out); continue; }
        if (line_.size() < MAX_SSE_LINE) line_ += p[i];
        else overflow_ = true;
    }
}

void SseParser::finish(std::vector<SseEvent> &out)
{
    if (!line_.empty()) handle(out);
    line_.clear();
    handle(out);                                               // as a blank line: dispatch what's pending
}
