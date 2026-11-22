#include "relay.h"
#include <stdlib.h>
#include <string.h>

static std::string strip(const std::string &s)             // Python's str.strip() for ASCII whitespace
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static const char *const RELAY_NAME = "Plus Modem relay";
static const char *const DEFAULT_SYSTEM =
    "You are chatting with someone who types on a 1986 Apple Macintosh Plus over a "
    "serial line. Its screen is 512 by 342 pixels, black and white. Answer in plain "
    "text with short paragraphs: no tables, no emoji, no wide ASCII art. Keep answers "
    "brief (under 150 words) unless asked for more.";

AiRelay::AiRelay(Config &cfg, Device &dev)
    : cfg_(cfg), dev_(dev), conn_(false), closing_(false), outPos_(0), overflow_(false), connMs_(0), lastMs_(0),
      st_(0), wrong_(0), dayStart_(0), dayCount_(0), reply_(0), lastFlush_(0), sent_(0)
{
    for (int i = 0; i < NSESSIONS; i++) sessions_[i].used = sessions_[i].attached = false;
    dayStart_ = dev.ms();
}

bool AiRelay::open()
{
    if (conn_) return false;
    conn_ = true;
    closing_ = false;
    out_.clear();
    outPos_ = 0;
    in_.clear();
    overflow_ = false;
    st_ = 0;
    wrong_ = 0;
    more_.clear();
    connMs_ = lastMs_ = dev_.ms();
    send(std::string("ASKAI 1 ") + RELAY_NAME);
    return true;
}

bool AiRelay::alive()
{
    return conn_ && !(closing_ && available() == 0);       // hang up once the last line has been read
}

int AiRelay::read(uint8_t *buf, int max)
{
    int n = available();
    if (n > max) n = max;
    memcpy(buf, out_.data() + outPos_, n);
    outPos_ += n;
    if (outPos_ == out_.size()) { out_.clear(); outPos_ = 0; }
    return n;
}

void AiRelay::hangup()
{
    if (!conn_) return;
    if (reply_) { delete reply_; reply_ = 0; }
    if (st_) { st_->attached = false; st_->lastSeen = dev_.ms(); }
    st_ = 0;
    conn_ = false;
    out_.clear();
    outPos_ = 0;
}

std::string AiRelay::describe()
{
    if (!st_) return "AI relay (waiting for HELLO)";
    const ModelDef *m = sessionModel();
    return "AI relay, session " + st_->id + ", model " + (m ? m->id : "-") + (reply_ ? ", replying" : "");
}

void AiRelay::send(const std::string &utf8)
{
    out_ += tc::utf8ToMac(utf8);
    out_ += '\r';
}

void AiRelay::bye(const char *line)
{
    send(line);
    closing_ = true;
}

int AiRelay::write(const uint8_t *p, int n)
{
    if (!conn_ || closing_) return n;
    for (int i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r' || c == '\n') {
            std::string l;
            l.swap(in_);
            if (overflow_) { overflow_ = false; send("ERR TOOLONG That line was too long."); }
            else if (trim(l).size()) line(l);
            if (closing_) break;
        } else if (in_.size() < MAX_LINE) {
            in_ += c;
        } else {
            overflow_ = true;
        }
    }
    return n;
}

// ------------------------------------------------------------------------ commands
void AiRelay::line(const std::string &mac)
{
    std::string l = tc::macToUtf8(mac);
    size_t a = l.find_first_not_of(' '), b = l.find_last_not_of(' ');
    l = a == std::string::npos ? "" : l.substr(a, b - a + 1);
    size_t sp = l.find(' ');
    std::string cmd = upper(l.substr(0, sp)), arg = sp == std::string::npos ? "" : l.substr(sp + 1);
    lastMs_ = dev_.ms();
    command(cmd, arg);
}

void AiRelay::command(const std::string &cmd, const std::string &arg)
{
    if (cmd == "PING") { send("PONG"); return; }
    if (cmd == "BYE") { stopReply(); bye("OK BYE"); return; }
    if (cmd == "HELLO") { hello(arg); return; }
    if (!st_) { send("ERR PROTO Send HELLO first."); return; }
    st_->lastSeen = dev_.ms();
    if (cmd == "STOP") {
        if (!stopReply()) send("OK STOP");
        return;
    }
    if (reply_) { send("ERR BUSY A reply is still coming; send STOP to end it."); return; }
    if (cmd == "MODELS") {
        int n = 0;
        for (size_t i = 0; i < cfg_.models.size(); i++) {
            const ModelDef &m = cfg_.models[i];
            if (!cfg_.usable(m)) continue;
            send("MODEL " + m.id + " " + m.tag + " " + m.label);
            n++;
        }
        const ModelDef *cur = sessionModel();
        send(fmt("OK MODELS %d ", n) + (cur ? cur->id : "-"));
    } else if (cmd == "USE") {
        const ModelDef *m = cfg_.model(trim(arg));
        if (!m) { send("ERR MODEL No model called " + trim(arg).substr(0, 40) + " here."); return; }
        st_->model = m->id;
        send("OK USE " + m->id + " " + m->tag + " " + m->label);
    } else if (cmd == "SYSTEM") {
        std::string s = trim(arg);
        st_->hasSystem = !s.empty();
        st_->system = s;
        send("OK SYSTEM");
    } else if (cmd == "NEW") {
        st_->history.clear();
        more_.clear();
        send("OK NEW");
    } else if (cmd == "MORE") {
        size_t total = arg.size();
        for (size_t i = 0; i < more_.size(); i++) total += more_[i].size();
        if (total > MAX_TURN) { more_.clear(); send("ERR TOOLONG That message is too long."); return; }
        more_.push_back(arg);
        send("OK MORE");
    } else if (cmd == "ASK") {
        ask(arg);
    } else {
        send("ERR PROTO Unknown command " + cmd.substr(0, 20) + ".");
    }
}

void AiRelay::hello(const std::string &arg)
{
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < arg.size()) {
        size_t j = arg.find(' ', i);
        if (j == std::string::npos) j = arg.size();
        if (j > i) parts.push_back(arg.substr(i, j - i));
        i = j + 1;
    }
    if (st_) { send("ERR PROTO Already said HELLO."); return; }
    if (parts.empty() || parts[0].find_first_not_of("0123456789") != std::string::npos || atoi(parts[0].c_str()) < 1) {
        send("ERR PROTO HELLO needs a version: HELLO 1 <pin>.");
        return;
    }
    std::string pin = parts.size() > 1 && parts[1] != "-" ? parts[1] : "";
    if (!cfg_.pin.empty() && pin != cfg_.pin) {
        if (++wrong_ >= PIN_TRIES) { bye("BYE PIN Wrong PIN too many times."); return; }
        send("ERR PIN Wrong PIN.");
        return;
    }
    uint32_t now = dev_.ms();
    for (int k = 0; k < NSESSIONS; k++)                     // forget old detached sessions
        if (sessions_[k].used && !sessions_[k].attached && now - sessions_[k].lastSeen > RESUME_MIN * 60000UL)
            sessions_[k].used = false;
    std::string sid = parts.size() > 2 ? upper(parts[2]) : "";
    Session *s = 0;
    for (int k = 0; k < NSESSIONS && !sid.empty(); k++)
        if (sessions_[k].used && !sessions_[k].attached && sessions_[k].id == sid) s = &sessions_[k];
    bool resumed = s != 0;
    if (!s) {
        for (int k = 0; k < NSESSIONS && !s; k++) if (!sessions_[k].used) s = &sessions_[k];
        for (int k = 0; k < NSESSIONS && !s; k++)            // all in use: replace the oldest detached one
            if (!sessions_[k].attached && (!s || sessions_[k].lastSeen < s->lastSeen)) s = &sessions_[k];
        if (!s) s = &sessions_[0];
        s->used = true;
        s->id = fmt("%08X", (unsigned)dev_.random32());
        const ModelDef *d = cfg_.defaultUsable();
        s->model = d ? d->id : "";
        s->hasSystem = false;
        s->system.clear();
        s->history.clear();
        s->recent.clear();
    }
    s->attached = true;
    s->lastSeen = now;
    st_ = s;
    const ModelDef *m = sessionModel();
    int turns = 0;
    for (size_t k = 0; k < s->history.size(); k++) turns += s->history[k].user;
    send("OK HELLO " + s->id + (resumed ? " RESUMED " : " NEW ") + fmt("%d ", turns) + (m ? m->id : "-"));
}

const ModelDef *AiRelay::sessionModel()
{
    if (!st_) return 0;
    const ModelDef *m = cfg_.model(st_->model);
    if (!m) {                                                 // its provider lost its key: the default
        m = cfg_.defaultUsable();
        st_->model = m ? m->id : "";
    }
    return m;
}

std::string AiRelay::limitReason()
{
    uint32_t now = dev_.ms();
    std::vector<uint32_t> &r = st_->recent;
    while (!r.empty() && now - r[0] > 60000) r.erase(r.begin());
    if ((int)r.size() >= PER_MINUTE) return fmt("Slow down: at most %d replies a minute.", (int)PER_MINUTE);
    if (now - dayStart_ > 86400000UL) { dayStart_ = now; dayCount_ = 0; }
    if (dayCount_ >= PER_DAY) return fmt("The modem's daily limit of %d replies is used up.", (int)PER_DAY);
    r.push_back(now);
    dayCount_++;
    return "";
}

void AiRelay::ask(const std::string &arg)
{
    std::string text;
    for (size_t i = 0; i < more_.size(); i++) text += more_[i] + "\n";
    text = strip(text + arg);
    more_.clear();
    const ModelDef *m = sessionModel();
    if (text.empty()) { send("ERR PROTO Nothing to send."); return; }
    if (text.size() > MAX_TURN) { send(fmt("ERR TOOLONG That message is too long (at most %d characters).", (int)MAX_TURN)); return; }
    if (!m) { send("ERR MODEL No model is available: set an API key from the MacBook (setup.py key)."); return; }
    std::string why = limitReason();
    if (!why.empty()) { send("ERR LIMIT " + why); return; }

    // drop the oldest pairs until the history and the new turn fit (relay.py trimmed_history)
    std::vector<Msg> &h = st_->history;
    size_t total = text.size();
    for (size_t i = 0; i < h.size(); i++) total += h[i].text.size();
    bool dropped = false;
    while (!h.empty() && total > MAX_HISTORY) {
        size_t k = h.size() >= 2 ? 2 : 1;
        for (size_t i = 0; i < k; i++) total -= h[i].text.size();
        h.erase(h.begin(), h.begin() + k);
        dropped = true;
    }
    if (dropped) send("NOTE Older messages were forgotten to make room.");
    send("BEGIN " + m->id + " " + m->tag);

    Msg user = {true, text};
    h.push_back(user);                                       // sent with the request; removed again below
    ReplyRequest rq;
    rq.model = *m;
    rq.prov = providerIndex(m->provider);
    rq.key = cfg_.keys[rq.prov];
    rq.url = cfg_.url(rq.prov);
    rq.system = st_->hasSystem ? st_->system : DEFAULT_SYSTEM;
    rq.msgs = &h;
    rq.maxTokens = MAX_REPLY_TOKENS;
    reply_ = startReply(rq, dev_);
    h.pop_back();
    askText_ = text;
    replyText_.clear();
    ps_.reset();
    pending_.clear();
    sent_ = 0;
    lastFlush_ = dev_.ms();
}

// ------------------------------------------------------------------------ streaming a reply
void AiRelay::poll()
{
    if (!conn_) return;
    uint32_t now = dev_.ms();
    if (!closing_) {
        if (!st_ && now - connMs_ > HELLO_S * 1000UL) { bye("BYE IDLE No HELLO received."); return; }
        if (st_ && !reply_ && now - lastMs_ > IDLE_MIN * 60000UL) { bye("BYE IDLE Idle for too long."); return; }
    }
    if (reply_) pumpReply();
}

long AiRelay::emit(std::vector<tc::Ev> &evs)
{
    long n = 0;
    tc::pack(evs);
    for (size_t i = 0; i < evs.size(); i++) {
        if (evs[i].kind == 'N') {
            out_ += "N\r";
            n += 2;
        } else {
            std::string b = tc::toMac(evs[i].text);
            if (b.empty()) continue;
            out_ += "T " + b + "\r";
            n += (long)b.size() + 3;
        }
    }
    evs.clear();
    return n;
}

void AiRelay::pumpReply()
{
    if (available() > 512) return;                           // the Mac is behind: let the line drain first
    std::string text;
    int st = reply_->poll(text, 1024);
    if (!text.empty()) {
        replyText_ += text;
        ps_.feed(text.data(), text.size(), pending_);
    }
    if (st == Reply::FAILED) { endReply(true); return; }
    if (st == Reply::DONE) { endReply(false); return; }
    size_t tchars = 0;
    for (size_t i = 0; i < pending_.size(); i++) tchars += pending_[i].text.size();
    uint32_t now = dev_.ms();
    if (!pending_.empty() && (now - lastFlush_ >= FLUSH_MS || tchars >= 120)) {
        sent_ += emit(pending_);
        lastFlush_ = now;
    } else if (pending_.empty()) {
        lastFlush_ = now;
    }
}

void AiRelay::endReply(bool failed)
{
    Reply *r = reply_;
    reply_ = 0;
    if (failed) {
        send("ERR " + r->errCode + " " + r->errMsg);
    } else {
        sent_ += emit(pending_);
        std::vector<tc::Ev> fin;
        ps_.finish(fin);
        sent_ += emit(fin);
        bool blank = strip(replyText_).empty();
        if (r->stop == "refused" && blank) send("NOTE The model declined to answer that.");
        if (!blank && r->stop != "refused") {
            Msg u = {true, askText_}, a = {false, replyText_};
            st_->history.push_back(u);
            st_->history.push_back(a);
        }
        send("END " + r->stop + fmt(" %ld %ld", r->tokens, sent_));
    }
    delete r;
    replyText_.clear();
}

bool AiRelay::stopReply()
{
    if (!reply_) return false;
    reply_->cancel();
    while (reply_) {                                          // a cancelled reply ends at its next poll
        std::string text;
        int st = reply_->poll(text, 1 << 20);
        if (!text.empty()) { replyText_ += text; ps_.feed(text.data(), text.size(), pending_); }
        if (st == Reply::FAILED) endReply(true);
        else if (st == Reply::DONE) endReply(false);
    }
    return true;
}
