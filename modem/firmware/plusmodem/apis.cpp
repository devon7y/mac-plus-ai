#include "apis.h"
#include <string.h>
#include "json.h"

const char *const ANTHROPIC_VERSION = "2026-10-21";
static const char *const FALLBACK_BETA = "server-side-fallback-2026-07-01";
static const char *const USER_AGENT = "PlusModem/" PM_VERSION " (+https://github.com/devon7y/mac-plus-ai)";
static const uint32_t TIMEOUT_MS = 120000;                  // no data from the provider for this long: give up

bool wantsFallbacks(const std::string &m)
{
    return m.compare(0, 15, "claude-opus-5-5") == 0 || m.compare(0, 17, "claude-sonnet-5-5") == 0;
}

std::string shortMsg(const std::string &s, size_t n)       // providers.py _short: one line, at most n characters
{
    std::string o;
    bool blank = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { blank = !o.empty(); continue; }
        if (blank) o += ' ';
        blank = false;
        o += c;
    }
    size_t chars = 0, cut = std::string::npos;
    for (size_t i = 0; i < o.size(); i++) {
        if (((unsigned char)o[i] & 0xC0) == 0x80) continue;
        if (chars == n - 1) cut = i;
        chars++;
    }
    if (chars <= n) return o;
    return o.substr(0, cut) + "\xE2\x80\xA6";                 // "…"
}

// ------------------------------------------------------------------------ requests
// The body is written twice: once to count its length, once to send it in 1 KB pieces.
struct Sink {
    Transport *t;                   // null: only count
    size_t count;
    std::string buf;
    bool ok;
    void put(const std::string &s)
    {
        count += s.size();
        if (!t) return;
        buf += s;
        if (buf.size() >= 1024) flush();
    }
    void flush()
    {
        if (t && !buf.empty() && ok) ok = t->send(buf.data(), buf.size());
        buf.clear();
    }
};

static std::string q(const std::string &s)
{
    std::string o;
    jquote(o, s);
    return o;
}

static void writeBody(const ReplyRequest &rq, Sink &s)
{
    ProvType type = PROVIDERS[rq.prov].type;
    const std::vector<Msg> &ms = *rq.msgs;
    std::string n = fmt("%d", rq.maxTokens);
    if (type == P_ANTHROPIC) {
        s.put("{\"model\":" + q(rq.model.model) + ",\"max_tokens\":" + n + ",\"stream\":true");
        if (!rq.system.empty()) s.put(",\"system\":" + q(rq.system));
        if (wantsFallbacks(rq.model.model)) s.put(",\"fallbacks\":\"default\"");
        s.put(",\"messages\":[");
        for (size_t i = 0; i < ms.size(); i++)
            s.put(std::string(i ? "," : "") + "{\"role\":\"" + (ms[i].user ? "user" : "assistant") + "\",\"content\":" + q(ms[i].text) + "}");
        s.put("]}");
    } else if (type == P_GEMINI) {
        s.put("{\"contents\":[");
        for (size_t i = 0; i < ms.size(); i++)
            s.put(std::string(i ? "," : "") + "{\"role\":\"" + (ms[i].user ? "user" : "model") + "\",\"parts\":[{\"text\":" + q(ms[i].text) + "}]}");
        s.put("],\"generationConfig\":{\"maxOutputTokens\":" + n + "}");
        if (!rq.system.empty()) s.put(",\"systemInstruction\":{\"parts\":[{\"text\":" + q(rq.system) + "}]}");
        s.put("}");
    } else {                        // OpenAI Chat Completions and compatible servers
        s.put("{\"model\":" + q(rq.model.model) + ",\"stream\":true,\"messages\":[");
        bool first = true;
        if (!rq.system.empty()) { s.put("{\"role\":\"system\",\"content\":" + q(rq.system) + "}"); first = false; }
        for (size_t i = 0; i < ms.size(); i++, first = false)
            s.put(std::string(first ? "" : ",") + "{\"role\":\"" + (ms[i].user ? "user" : "assistant") + "\",\"content\":" + q(ms[i].text) + "}");
        s.put("]");
        if (type == P_OPENAI) s.put(",\"max_completion_tokens\":" + n + ",\"stream_options\":{\"include_usage\":true}");
        else s.put(",\"max_tokens\":" + n);
        s.put("}");
    }
}

bool sendRequest(const ReplyRequest &rq, const Url &u, Transport &t)
{
    ProvType type = PROVIDERS[rq.prov].type;
    Sink count = {0, 0, std::string(), true};
    writeBody(rq, count);
    std::string path, h;
    if (type == P_ANTHROPIC) {
        path = u.path + "/v1/messages";
        h = "x-api-key: " + rq.key + "\r\nanthropic-version: " + ANTHROPIC_VERSION + "\r\n";
        if (wantsFallbacks(rq.model.model)) h += std::string("anthropic-beta: ") + FALLBACK_BETA + "\r\n";
    } else if (type == P_GEMINI) {
        path = u.path + "/v1beta/models/" + rq.model.model + ":streamGenerateContent?alt=sse";
        h = "x-goog-api-key: " + rq.key + "\r\n";
    } else {
        path = u.path + "/chat/completions";
        if (!rq.key.empty()) h = "Authorization: Bearer " + rq.key + "\r\n";
        if (std::string(PROVIDERS[rq.prov].name) == "openrouter") h += "X-Title: Ask AI (Mac Plus)\r\n";
    }
    bool defPort = u.port == (u.tls ? 443 : 80);
    std::string head = "POST " + (path[0] == '/' ? path : "/" + path) + " HTTP/1.1\r\nHost: " + u.host +
                       (defPort ? "" : fmt(":%u", u.port)) + "\r\n" + h +
                       "Content-Type: application/json\r\nAccept: text/event-stream\r\nUser-Agent: " + USER_AGENT +
                       "\r\nContent-Length: " + fmt("%u", (unsigned)count.count) + "\r\nConnection: close\r\n\r\n";
    Sink s = {&t, 0, std::string(), true};
    s.put(head);
    writeBody(rq, s);
    s.flush();
    return s.ok;
}

// ------------------------------------------------------------------------ responses
bool readEvent(ProvType type, const SseEvent &ev, std::string &text, Reply &r, bool &end)
{
    const std::string &d = ev.data;
    if (type == P_ANTHROPIC) {
        JVal tv = jget(d, "type");
        if (!jget(d, "").type) return true;                  // not JSON: skipped
        std::string t = tv.type ? jstr(tv) : ev.event;
        if (t == "content_block_delta") {
            if (jstr(d, "delta.type") == "text_delta") text += jstr(d, "delta.text");
        } else if (t == "message_delta") {
            std::string sr = jstr(d, "delta.stop_reason");
            if (!sr.empty()) r.stop = sr == "max_tokens" ? "length" : sr == "refusal" ? "refused" : "done";
            JVal ot = jget(d, "usage.output_tokens");
            if (ot.type == 'n') r.tokens = jnum(ot);
        } else if (t == "message_stop") {
            end = true;
        } else if (t == "error") {
            std::string et = jstr(d, "error.type"), msg = jstr(d, "error.message");
            if (et == "overloaded_error") { r.errCode = "PROVIDER"; r.errMsg = "Anthropic is overloaded right now; try again soon."; }
            else if (et == "rate_limit_error") { r.errCode = "LIMIT"; r.errMsg = "Anthropic's rate limit was hit; try again in a minute."; }
            else { r.errCode = "PROVIDER"; r.errMsg = "Anthropic error: " + shortMsg(msg.empty() ? et : msg, 160); }
            return false;
        }
        return true;
    }
    if (type == P_GEMINI) {
        if (!jget(d, "").type) return true;
        JVal e = jget(d, "error");
        if (e.type && e.type != 'z') {
            r.errCode = "PROVIDER";
            r.errMsg = "Gemini error: " + shortMsg(jstr(d, "error.message"), 160);
            return false;
        }
        if (!jstr(d, "promptFeedback.blockReason").empty()) r.stop = "refused";
        for (int c = 0;; c++) {
            std::string base = fmt("candidates.%d", c);
            if (!jget(d, base.c_str()).type) break;
            for (int k = 0;; k++) {
                std::string part = base + fmt(".content.parts.%d", k);
                if (!jget(d, part.c_str()).type) break;
                JVal th = jget(d, (part + ".thought").c_str());
                if (th.type != 't') text += jstr(d, (part + ".text").c_str());
            }
            std::string fr = jstr(d, (base + ".finishReason").c_str());
            if (!fr.empty()) r.stop = fr == "STOP" ? "done" : fr == "MAX_TOKENS" ? "length" : "refused";
        }
        JVal ct = jget(d, "usageMetadata.candidatesTokenCount");
        if (ct.type == 'n') r.tokens = jnum(ct);
        return true;
    }
    // OpenAI style
    if (trim(d) == "[DONE]") { end = true; return true; }
    if (!jget(d, "").type) return true;
    JVal e = jget(d, "error");
    if (e.type && e.type != 'z' && e.type != 'f') {
        r.errCode = "PROVIDER";
        r.errMsg = "Provider error: " + shortMsg(e.type == 's' ? jstr(e) : jstr(d, "error.message"), 160);
        return false;
    }
    for (int c = 0;; c++) {
        std::string base = fmt("choices.%d", c);
        if (!jget(d, base.c_str()).type) break;
        text += jstr(d, (base + ".delta.content").c_str());
        std::string fr = jstr(d, (base + ".finish_reason").c_str());
        if (!fr.empty()) r.stop = fr == "length" ? "length" : fr == "content_filter" ? "refused" : "done";
    }
    JVal ct = jget(d, "usage.completion_tokens");
    if (ct.type == 'n') r.tokens = jnum(ct);
    return true;
}

void httpError(int status, const std::string &body, std::string &code, std::string &msg)
{
    std::string detail;
    JVal all = jget(body, "");
    if (all.type) {
        JVal e = jget(body, "error");
        if (!e.type) e = all;                                  // e = j.get("error", j)
        std::string es(e.p, e.n);
        if (e.type == 'o') { detail = jstr(es, "message"); if (detail.empty()) detail = jstr(es, "type"); }
        else if (e.type == 'a') detail = jget(es, "0.error").type ? jstr(es, "0.error.message") : jstr(es, "0.message");
        else if (e.type == 's') detail = jstr(e);
    } else {
        detail = body.substr(0, 200);
    }
    if (status == 401 || status == 403) {
        code = "KEY";
        msg = fmt("The provider rejected the API key (HTTP %d). ", status) + shortMsg(detail, 120);
    } else if (status == 404) {
        code = "MODEL";
        msg = "The provider doesn't know this model (HTTP 404). " + shortMsg(detail, 120);
    } else if (status == 429) {
        code = "LIMIT";
        msg = "The provider's rate limit was hit; try again in a minute. " + shortMsg(detail, 100);
    } else if (status == 500 || status == 502 || status == 503 || status == 504 || status == 529) {
        code = "PROVIDER";
        msg = fmt("The provider is busy or down (HTTP %d); try again soon.", status);
    } else {
        code = "PROVIDER";
        msg = fmt("Provider error (HTTP %d): ", status) + shortMsg(detail, 160);
    }
    msg = trim(msg);
}

class FailedReply : public Reply {
public:
    FailedReply(const char *code, const std::string &msg) { errCode = code; errMsg = msg; }
    int poll(std::string &, size_t) { return FAILED; }
    void cancel() {}
};

class HttpReply : public Reply {
public:
    HttpReply(Device &dev, Transport &t, ProvType type) : dev_(dev), t_(t), type_(type), state_(RUNNING), cancel_(false)
    {
        last_ = dev.ms();
    }
    ~HttpReply() { t_.close(); }
    void cancel() { cancel_ = true; }

    int poll(std::string &text, size_t max)
    {
        if (state_ != RUNNING) return state_;
        if (cancel_) { stop = "stopped"; return finish(DONE); }
        uint8_t buf[512];
        size_t got = 0;
        bool closed = false, end = false;
        while (got < max && state_ == RUNNING) {
            int n = t_.recv(buf, sizeof buf);
            if (n == 0) break;
            if (n < 0) { closed = true; break; }
            got += n;
            last_ = dev_.ms();
            body_.clear();
            if (!resp_.feed(buf, n, body_)) return fail("NET", "The provider's answer wasn't HTTP.");
            if (!resp_.headersDone()) continue;
            if (resp_.status != 200) {
                if (err_.size() < 4000) err_ += body_;
            } else {
                events_.clear();
                sse_.feed(body_.data(), body_.size(), events_);
                if (!events(text, end)) return state_;
            }
            if (resp_.done()) closed = true;
            if (end || closed) break;
        }
        if (end) return finish(DONE);
        if (closed) {
            if (!resp_.headersDone()) return fail("NET", "The provider closed the connection.");
            if (resp_.status != 200) {
                std::string code, msg;
                httpError(resp_.status, err_, code, msg);
                return fail(code.c_str(), msg);
            }
            events_.clear();
            sse_.finish(events_);
            if (!events(text, end)) return state_;
            return finish(DONE);
        }
        if (dev_.ms() - last_ > TIMEOUT_MS) return fail("NET", "The provider stopped answering.");
        return state_;
    }

private:
    Device &dev_;
    Transport &t_;
    ProvType type_;
    int state_;
    bool cancel_;
    uint32_t last_;
    HttpResponse resp_;
    SseParser sse_;
    std::string body_, err_;
    std::vector<SseEvent> events_;

    bool events(std::string &text, bool &end)
    {
        for (size_t i = 0; i < events_.size(); i++)
            if (!readEvent(type_, events_[i], text, *this, end)) { fail(errCode.c_str(), errMsg); return false; }
        return true;
    }
    int finish(int st)
    {
        t_.close();
        return state_ = st;
    }
    int fail(const char *code, const std::string &msg)
    {
        errCode = code;
        errMsg = msg;
        return finish(FAILED);
    }
};

// ------------------------------------------------------------------------ the mock provider
static const char *const MOCK_REPLIES[] = {
    "Hello from the mock provider! I'm a pretend model that lives inside the relay, so nothing here "
    "costs money.\n\nYou can ask me anything; I'll answer with **canned** text, but the words travel "
    "the whole way: relay, modem, serial cable and your Mac's screen.",
    "Here are three things a Macintosh Plus is good at:\n\n- Writing letters in *MacWrite*\n"
    "- Drawing in MacPaint\n- Chatting with a model through a modem\n\nThe Plus came out in January "
    "1986 with 1 MB of memory and an 800K floppy drive.",
    "A tiny program, as the relay shows code:\n\n```c\nint add(int a, int b)\n{\n    return a + b;\n}\n"
    "```\n\nCode blocks arrive indented by four spaces, and `inline code` loses its backticks.",
    "### A short answer\n\nYes. The [relay](https://example.com/relay) turns Markdown into plain "
    "text, keeps the conversation and sends only the new words over the line.",
};

static size_t utf8Chars(const std::string &s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) n += ((unsigned char)s[i] & 0xC0) != 0x80;
    return n;
}

static std::string utf8Prefix(const std::string &s, size_t chars)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80 && n++ == chars) return s.substr(0, i);
    }
    return s;
}

class MockReply : public Reply {
    // Canned replies with realistic timing. Triggers in the user's text, as in the Python relay:
    // !error (provider error), !long (about 4 KB), !slow, !key (missing key), !refuse, !cut (length limit).
public:
    MockReply(Device &dev, const ReplyRequest &rq) : dev_(dev), pos_(0), cancel_(false), mode_(0)
    {
        const std::vector<Msg> &ms = *rq.msgs;
        std::string text = ms.back().text, low = lower(text);
        int turn = 0;
        for (size_t i = 0; i < ms.size(); i++) turn += ms[i].user;
        cps_ = 250;
        t0_ = dev.ms();
        rng_ = 12345u + (uint32_t)turn * 2654435761u;
        if (low.find("!key") != std::string::npos) mode_ = 'k';
        else if (low.find("!error") != std::string::npos) mode_ = 'e';
        else if (low.find("!refuse") != std::string::npos) mode_ = 'r';
        if (low.find("!long") != std::string::npos) {
            for (int i = 1; i <= 40; i++)
                reply_ += fmt("%sSentence %d of a long reply, written to check scrolling and flow control on the serial line.", i > 1 ? " " : "", i);
        } else {
            std::string shown = utf8Chars(text) <= 60 ? text : utf8Prefix(text, 57) + "...";
            reply_ = "You said: \"" + shown + "\"\n\n" + MOCK_REPLIES[(turn - 1) % 4];
            if (!rq.model.model.empty()) reply_ += fmt("\n\n(Mock model: %s, turn %d.)", rq.model.model.c_str(), turn);
        }
        if (low.find("!slow") != std::string::npos) cps_ = 20;
        if (low.find("!cut") != std::string::npos) {
            reply_ = utf8Prefix(reply_, 120);
            stop = "length";
        }
    }
    void cancel() { cancel_ = true; }

    int poll(std::string &text, size_t)
    {
        if (mode_ == 'k') { errCode = "KEY"; errMsg = "No API key: the mock model pretends one is missing (you typed !key)."; return FAILED; }
        uint32_t t = dev_.ms() - t0_;
        if (cancel_) { stop = "stopped"; return DONE; }
        if (t < 600) return RUNNING;                          // latency before the first words
        if (mode_ == 'e') { errCode = "PROVIDER"; errMsg = "The mock provider failed on purpose (you typed !error)."; return FAILED; }
        if (mode_ == 'r') { stop = "refused"; return DONE; }
        while (pos_ < reply_.size() && (double)pos_ * 1000.0 <= (double)(t - 600) * cps_) {
            rng_ = rng_ * 1103515245u + 12345u;
            size_t n = 2 + (rng_ >> 16) % 11;                 // a token or three at a time
            text += reply_.substr(pos_, n);
            pos_ += n;
            tokens++;
        }
        return pos_ >= reply_.size() ? DONE : RUNNING;
    }

private:
    Device &dev_;
    std::string reply_;
    size_t pos_;
    bool cancel_;
    int mode_;
    double cps_;
    uint32_t t0_, rng_;
};

Reply *startReply(const ReplyRequest &rq, Device &dev)
{
    const ProviderDef &p = PROVIDERS[rq.prov];
    if (p.type == P_MOCK) return new MockReply(dev, rq);
    Url u;
    if (!parseUrl(rq.url, u))
        return new FailedReply("PROVIDER", std::string("No valid address for provider ") + p.name + ": set it with AT+URL from the MacBook.");
    if (p.needsKey && rq.key.empty())
        return new FailedReply("KEY", std::string("No API key for ") + p.name + ": set it from the MacBook (setup.py key " + p.name + ").");
    if (!dev.wifiUp()) return new FailedReply("NET", "The modem isn't on Wi-Fi.");
    Transport *t = dev.transport();
    std::string err;
    if (!t->open(u.host, u.port, u.tls, err)) {
        t->close();
        return new FailedReply("NET", "Couldn't reach the provider: " + shortMsg(err, 140));
    }
    if (!sendRequest(rq, u, *t)) {
        t->close();
        return new FailedReply("NET", "Couldn't reach the provider: the request couldn't be sent.");
    }
    return new HttpReply(dev, *t, p.type);
}
