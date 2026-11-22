// Plus Modem: the model providers of the built-in relay (a port of askai/relay/providers.py).
// A Reply streams one answer: text deltas (UTF-8) as they arrive, then a stop reason or an error.
// Neither the request nor the response is ever held whole in memory.
#pragma once
#include <string>
#include <vector>
#include "config.h"
#include "http.h"
#include "iface.h"

struct Msg { bool user; std::string text; };

struct ReplyRequest {
    ModelDef model;
    int prov;                       // index into PROVIDERS
    std::string key, url, system;
    const std::vector<Msg> *msgs;   // oldest first, ending with the new user turn
    int maxTokens;
};

class Reply {
public:
    enum { RUNNING, DONE, FAILED };
    Reply() : stop("done"), tokens(0) {}
    virtual ~Reply() {}
    // Appends new text to `text` (reading about `max` bytes from the network per call). DONE: `stop`
    // and `tokens` are set; FAILED: `errCode` (KEY MODEL LIMIT NET PROVIDER) and `errMsg`.
    virtual int poll(std::string &text, size_t max) = 0;
    virtual void cancel() = 0;      // the next poll() returns DONE with stop "stopped"
    std::string stop;               // done, length, refused, stopped
    long tokens;
    std::string errCode, errMsg;
};

// Starts a reply. For HTTP providers this connects and sends the request (blocking, a second or two).
Reply *startReply(const ReplyRequest &rq, Device &dev);

// The pieces, exposed for the host tests.
extern const char *const ANTHROPIC_VERSION;
bool wantsFallbacks(const std::string &model);              // Opus 5.5 and Sonnet 5.5 only
bool sendRequest(const ReplyRequest &rq, const Url &u, Transport &t);
bool readEvent(ProvType t, const SseEvent &ev, std::string &text, Reply &r, bool &end);   // false: error event
void httpError(int status, const std::string &body, std::string &code, std::string &msg);
std::string shortMsg(const std::string &s, size_t n);
