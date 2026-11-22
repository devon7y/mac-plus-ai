// Plus Modem: the built-in Ask AI relay (askai/relay/PROTOCOL.md), reached with ATDT ai.
// It is the far end of a modem line: the Mac writes protocol lines into it and reads the replies.
// A port of askai/relay/relay.py for one connection at a time.
#pragma once
#include <string>
#include <vector>
#include "apis.h"
#include "config.h"
#include "iface.h"
#include "textconv.h"

class AiRelay : public Remote {
public:
    AiRelay(Config &cfg, Device &dev);
    bool open();                    // a new connection; false if one is already open
    bool busy() const { return conn_; }

    // Remote
    bool alive();
    int available() { return (int)(out_.size() - outPos_); }
    int read(uint8_t *buf, int max);
    int write(const uint8_t *p, int n);
    void poll();
    void hangup();
    std::string describe();

    // limits (relay.example.toml defaults; the history is smaller to fit the ESP32's memory)
    enum {
        MAX_LINE = 2048, MAX_TURN = 2000, MAX_REPLY_TOKENS = 1024, MAX_HISTORY = 16000,
        PER_MINUTE = 8, PER_DAY = 400, PIN_TRIES = 3, HELLO_S = 120, IDLE_MIN = 30, RESUME_MIN = 60,
        FLUSH_MS = 120, NSESSIONS = 2
    };

private:
    struct Session {
        bool used, attached;
        std::string id, model, system;
        bool hasSystem;
        std::vector<Msg> history;
        std::vector<uint32_t> recent;          // times of recent replies (per-minute cap)
        uint32_t lastSeen;
    };
    Config &cfg_;
    Device &dev_;
    bool conn_, closing_;
    std::string out_;                          // Mac Roman bytes for the Mac
    size_t outPos_;
    std::string in_;                           // the line being received
    bool overflow_;
    uint32_t connMs_, lastMs_;
    Session sessions_[NSESSIONS];
    Session *st_;
    int wrong_;
    std::vector<std::string> more_;
    uint32_t dayStart_;
    int dayCount_;
    // the reply being streamed
    Reply *reply_;
    tc::PlainStream ps_;
    std::vector<tc::Ev> pending_;
    uint32_t lastFlush_;
    std::string replyText_, askText_;
    long sent_;

    void send(const std::string &utf8);       // one protocol line (converted to Mac Roman)
    void line(const std::string &mac);
    void command(const std::string &cmd, const std::string &arg);
    void hello(const std::string &arg);
    void ask(const std::string &arg);
    void pumpReply();
    long emit(std::vector<tc::Ev> &evs);
    void endReply(bool failed);
    bool stopReply();                         // true if a reply was running
    std::string limitReason();
    const ModelDef *sessionModel();
    void bye(const char *line);
};
