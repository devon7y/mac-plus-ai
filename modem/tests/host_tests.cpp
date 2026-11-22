// Host tests for the Plus Modem's portable core: the Hayes lines, the AI relay (with the mock model
// and with recorded provider streams), JSON, HTTP and SSE, and the settings helpers.
//     modem/tests/run_host_tests.sh
#include <stdio.h>
#include <string.h>
#include <map>
#include "../firmware/plusmodem/apis.h"
#include "../firmware/plusmodem/config.h"
#include "../firmware/plusmodem/http.h"
#include "../firmware/plusmodem/json.h"
#include "../firmware/plusmodem/modem.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { checks++; std::string _a = (a), _b = (b); if (_a != _b) { fails++; printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, show(_a).c_str(), show(_b).c_str()); } } while (0)
#define HAS(s, sub) do { checks++; std::string _s = (s), _u = (sub); if (_s.find(_u) == std::string::npos) { fails++; printf("FAIL %s:%d: %s lacks \"%s\"\n  in: %s\n", __FILE__, __LINE__, #s, show(_u).c_str(), show(_s).c_str()); } } while (0)
#define LACKS(s, sub) do { checks++; std::string _s = (s), _u = (sub); if (_s.find(_u) != std::string::npos) { fails++; printf("FAIL %s:%d: %s contains \"%s\"\n  in: %s\n", __FILE__, __LINE__, #s, show(_u).c_str(), show(_s).c_str()); } } while (0)

static std::string show(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = s[i];
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c < 32 || c > 126) o += fmt("\\x%02x", c);
        else o += (char)c;
    }
    return o;
}

// ------------------------------------------------------------------------ fakes
struct FakePort : Port {
    std::string in, out;
    size_t pos = 0;
    int roomv = 1 << 20;
    int read(uint8_t *b, int max) { int n = (int)std::min((size_t)max, in.size() - pos); memcpy(b, in.data() + pos, n); pos += n; return n; }
    int room() { return roomv; }
    void write(const uint8_t *p, int n) { out.append((const char *)p, n); }
    std::string take() { std::string s; s.swap(out); return s; }
    void type(const std::string &s) { in += s; }
};

struct EchoRemote : Remote {                        // a TCP server that echoes in upper case
    std::string buf;
    bool open = true;
    bool *deleted;
    explicit EchoRemote(bool *d) : deleted(d) {}
    bool alive() { return open || !buf.empty(); }
    int available() { return (int)buf.size(); }
    int read(uint8_t *b, int max) { int n = std::min(max, (int)buf.size()); memcpy(b, buf.data(), n); buf.erase(0, n); return n; }
    int write(const uint8_t *p, int n) { buf += upper(std::string((const char *)p, n)); return n; }
    void hangup() { *deleted = true; delete this; }
    std::string describe() { return "echo"; }
};

struct FakeTransport : Transport {
    std::string sent, host;
    uint16_t port = 0;
    bool tls = false, opened = false, closed = false, failOpen = false;
    std::vector<std::string> pieces;                // the response, handed out piece by piece
    size_t next = 0;
    bool open(const std::string &h, uint16_t p, bool t, std::string &err)
    {
        host = h; port = p; tls = t; sent.clear(); next = 0; closed = false;
        if (failOpen) { err = "host not found"; return false; }
        opened = true;
        return true;
    }
    bool send(const char *p, size_t n) { sent.append(p, n); return true; }
    int recv(uint8_t *b, size_t max)
    {
        if (next >= pieces.size()) return -1;
        std::string &s = pieces[next];
        size_t n = std::min(max, s.size());
        memcpy(b, s.data(), n);
        s.erase(0, n);
        if (s.empty()) next++;
        return (int)n;
    }
    void close() { closed = true; }
    void respond(const std::string &all, size_t step)  // split a whole response into pieces
    {
        pieces.clear();
        for (size_t i = 0; i < all.size(); i += step) pieces.push_back(all.substr(i, step));
    }
};

struct FakeDevice : Device {
    uint32_t now = 1000;
    uint32_t seed = 1;
    bool wifi = false;
    int saves = 0;
    uint32_t baud = 0;
    std::vector<std::string> logs;
    FakeTransport tr;
    bool echoDeleted = false;
    uint32_t ms() { return now; }
    uint32_t random32() { seed = seed * 1664525u + 1013904223u; return seed; }
    void log(const std::string &l) { logs.push_back(l); }
    bool wifiUp() { return wifi; }
    std::string wifiStatus() { return wifi ? "connected to Test" : "not connected"; }
    void wifiChanged() {}
    bool scan(std::vector<ScanEntry> &v) { ScanEntry e = {"Home", -50, 6, false, false}; v.push_back(e); return true; }
    Remote *dialTcp(const std::string &h, uint16_t) { return h == "echo.test" ? new EchoRemote(&echoDeleted) : 0; }
    Transport *transport() { return &tr; }
    void save(const Config &) { saves++; }
    void setBaud(uint32_t b) { baud = b; }
    std::string pinInfo() { return "RX=GPIO16 TX=GPIO17 (test)"; }
    bool loopback(std::vector<std::string> &o) { o.push_back("LOOPBACK OK"); return true; }
    bool tlsTest(const std::string &h, std::vector<std::string> &o) { o.push_back("TLS OK " + h); return true; }
    uint32_t freeHeap() { return 200000; }
    uint32_t minFreeHeap() { return 150000; }
};

struct Rig {
    Config cfg;
    FakeDevice dev;
    FakePort macPort, usbPort;
    Modem modem;
    Line mac, usb;
    Rig() : modem(cfg, dev), mac(modem, macPort, false, 16384), usb(modem, usbPort, true, 2048)
    {
        modem.attach(&mac, &usb);
        dev.now = 70000;                            // past the boot banner unless a test sets it
    }
    void run(uint32_t ms, uint32_t step = 10)
    {
        for (uint32_t t = 0; t < ms; t += step) { dev.now += step; modem.poll(); }
    }
    std::string mac_(const std::string &typed, uint32_t ms = 50) { macPort.type(typed); run(ms); return macPort.take(); }
    std::string usb_(const std::string &typed, uint32_t ms = 50) { usbPort.type(typed); run(ms); return usbPort.take(); }
};

// ------------------------------------------------------------------------ tests
static void testHelpers()
{
    std::vector<std::string> a;
    CHECK(splitArgs("\"Home Net\",\"pa\\\"ss,word\"", a) && a.size() == 2 && a[0] == "Home Net" && a[1] == "pa\"ss,word");
    CHECK(splitArgs("eduroam, user@example.edu ,secret", a) && a.size() == 3 && a[1] == "user@example.edu");
    CHECK(!splitArgs("\"open", a));
    std::string h;
    uint16_t p;
    CHECK(splitHostPort("192.168.1.20:6801", h, p, 23) && h == "192.168.1.20" && p == 6801);
    CHECK(splitHostPort("bbs.example.com", h, p, 23) && p == 23);
    CHECK(splitHostPort(" telehack.com 23 ", h, p, 0) && h == "telehack.com" && p == 23);
    CHECK(!splitHostPort("host:99999", h, p, 23));
    CHECK(!splitHostPort(":80", h, p, 23));
    Url u;
    CHECK(parseUrl("https://api.anthropic.com", u) && u.tls && u.host == "api.anthropic.com" && u.port == 443 && u.path == "");
    CHECK(parseUrl("http://192.168.1.20:11434/v1/", u) && !u.tls && u.port == 11434 && u.path == "/v1");
    CHECK(!parseUrl("ftp://x", u));
    CHECK_EQ(maskKey("sk-ant-api03-abcdefghijklmnop1234"), "sk-...1234");
    CHECK_EQ(maskKey("short"), "****");
    CHECK_EQ(maskLine("AT+KEY anthropic=sk-secret"), "AT+KEY anthropic=*********");
    CHECK_EQ(maskLine("AT+WIFI=\"Home, Net\",\"secret\""), "AT+WIFI=\"Home, Net\",********");
    CHECK_EQ(maskLine("AT+WIFIEAP=\"eduroam\",\"id\",\"user\",\"pw\""), "AT+WIFIEAP=\"eduroam\",\"id\",\"user\",****");
    CHECK_EQ(maskLine("at+pin=1234"), "at+pin=****");
    CHECK_EQ(maskLine("AT+WIFILIST"), "AT+WIFILIST");
    CHECK_EQ(maskLine("ATDT ai"), "ATDT ai");

    Config c;
    WifiNet n1 = {false, "Home", "p,a\"ss", "", ""}, n2 = {true, "eduroam", "pw", "id@example.edu", "id@example.edu"};
    c.nets.push_back(n1);
    c.nets.push_back(n2);
    Config d;
    d.parseNets(c.netsText());
    CHECK(d.nets.size() == 2 && d.nets[0].pass == "p,a\"ss" && d.nets[1].eap && d.nets[1].user == "id@example.edu");
    d.parseModels(c.modelsText());
    CHECK(d.models.size() == c.models.size() && d.models[0].model == "claude-opus-5-5");
    CHECK(c.firstUsable() && c.firstUsable()->id == "mock");        // no keys: only the mock
    c.keys[0] = "k";
    CHECK(c.defaultUsable()->id == "claude");
}

static void testJson()
{
    std::string j = "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \\\"there\\\"\\n\\u00e9\\ud83d\\ude00\"},"
                    "\"choices\":[{\"x\":[1,2,{\"y\":\"z\"}]},{\"delta\":{\"content\":\"second\"}}],\"n\":null}";
    CHECK_EQ(jstr(j, "type"), "content_block_delta");
    CHECK_EQ(jstr(j, "delta.text"), "Hi \"there\"\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(jnum(j, "index") == 1);
    CHECK_EQ(jstr(j, "choices.0.x.2.y"), "z");
    CHECK_EQ(jstr(j, "choices.1.delta.content"), "second");
    CHECK(jget(j, "choices.2").type == 0);
    CHECK(jget(j, "n").type == 'z');
    CHECK(jget(j, "missing.x").type == 0);
    CHECK(jget("not json", "").type == 0);
    CHECK(jget("{\"a\":1} x", "").type == 0);
    CHECK(jget(" [true, false, null, -1.5e3] ", "3").type == 'n');
    CHECK(jget("{\"a\":", "a").type == 0);
    std::string q;
    jquote(q, "a\"b\\c\n\x01\xC3\xA9");
    CHECK_EQ(q, "\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\"");
}

static std::string sse(const char *event, const std::string &data) { return std::string(event ? std::string("event: ") + event + "\n" : "") + "data: " + data + "\n\n"; }

static std::string chunked(const std::string &body, size_t chunk)
{
    std::string o = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (size_t i = 0; i < body.size(); i += chunk) {
        std::string c = body.substr(i, chunk);
        o += fmt("%x\r\n", (unsigned)c.size()) + c + "\r\n";
    }
    return o + "0\r\n\r\n";
}

static void testHttpSse()
{
    std::string resp = chunked("event: a\ndata: {\"x\":1}\n\n: keep-alive\r\n\r\ndata: one\r\ndata: two\n\n", 7);
    for (size_t step = 1; step <= resp.size(); step += (step < 8 ? 1 : 37)) {
        HttpResponse h;
        SseParser sse;
        std::vector<SseEvent> ev;
        for (size_t i = 0; i < resp.size(); i += step) {
            std::string body;
            CHECK(h.feed((const uint8_t *)resp.data() + i, std::min(step, resp.size() - i), body));
            sse.feed(body.data(), body.size(), ev);
        }
        CHECK(h.done() && h.status == 200);
        CHECK(ev.size() == 2 && ev[0].event == "a" && ev[0].data == "{\"x\":1}" && ev[1].event == "message" && ev[1].data == "one\ntwo");
    }
    HttpResponse h;
    std::string body;
    std::string r2 = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 401 Unauthorized\r\nContent-Length: 5\r\n\r\nabcdeXYZ";
    h.feed((const uint8_t *)r2.data(), r2.size(), body);
    CHECK(h.status == 401 && h.done() && body == "abcde");
}

static std::string anthropicStream(const std::string &stop)
{
    return sse("message_start", "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{\"input_tokens\":12,\"output_tokens\":1}}}") +
           sse("content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}") +
           sse("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"\"}}") +
           sse("ping", "{\"type\":\"ping\"}") +
           sse("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"**Hello** \\u201cMac\\u201d\"}}") +
           sse("content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\" \\ud83d\\ude00 Plus!\\n\\n- one\\n- two\"}}") +
           sse("message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" + stop + "\"},\"usage\":{\"output_tokens\":17}}") +
           sse("message_stop", "{\"type\":\"message_stop\"}");
}

// the reply text as the Mac shows it: T lines joined, N as a line break
static std::string plain(const std::string &o)
{
    std::string t;
    size_t a = 0;
    while (a < o.size()) {
        size_t b = o.find('\r', a);
        if (b == std::string::npos) b = o.size();
        std::string l = o.substr(a, b - a);
        if (l.compare(0, 2, "T ") == 0) t += l.substr(2);
        else if (l == "N") t += "\n";
        a = b + 1;
    }
    return t;
}

// drive the relay over the USB line: dial ai, HELLO, then whatever the test sends
static std::string relayExchange(Rig &r, const std::string &lines, uint32_t ms = 3000)
{
    r.usbPort.type(lines);
    std::string got;
    for (uint32_t t = 0; t < ms; t += 10) { r.dev.now += 10; r.modem.poll(); got += r.usbPort.take(); }
    return got;
}

static void testProviders()
{
    // request bodies and headers
    std::vector<Msg> ms;
    Msg m1 = {true, "Hi \"Mac\"\n"};
    ms.push_back(m1);
    FakeTransport t;
    ReplyRequest rq;
    rq.model = Config::builtinModels()[0];          // claude-opus-5-5
    rq.prov = 0;
    rq.key = "sk-test";
    rq.url = "https://api.anthropic.com";
    rq.system = "Be brief.";
    rq.msgs = &ms;
    rq.maxTokens = 1024;
    Url u;
    parseUrl(rq.url, u);
    CHECK(sendRequest(rq, u, t));
    size_t hb = t.sent.find("\r\n\r\n");
    std::string head = t.sent.substr(0, hb), body = t.sent.substr(hb + 4);
    HAS(head, "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\n");
    HAS(head, "x-api-key: sk-test\r\n");
    HAS(head, "anthropic-version: 2026-10-21\r\n");
    HAS(head, "anthropic-beta: server-side-fallback-2026-07-01\r\n");
    HAS(head, fmt("Content-Length: %u\r\n", (unsigned)body.size()).c_str());
    CHECK_EQ(jstr(body, "model"), "claude-opus-5-5");
    CHECK(jnum(body, "max_tokens") == 1024 && jget(body, "stream").type == 't');
    CHECK_EQ(jstr(body, "fallbacks"), "default");
    CHECK_EQ(jstr(body, "messages.0.content"), "Hi \"Mac\"\n");
    CHECK_EQ(jstr(body, "system"), "Be brief.");
    LACKS(body, "temperature");
    LACKS(body, "top_p");
    LACKS(body, "top_k");
    LACKS(body, "thinking");
    rq.model = Config::builtinModels()[2];          // haiku: no fallbacks
    t.sent.clear();
    sendRequest(rq, u, t);
    LACKS(t.sent, "anthropic-beta");
    LACKS(t.sent, "fallbacks");
    rq.model = Config::builtinModels()[3];          // gpt-5
    rq.prov = 1;
    parseUrl("https://api.openai.com/v1", u);
    t.sent.clear();
    sendRequest(rq, u, t);
    HAS(t.sent, "POST /v1/chat/completions HTTP/1.1");
    HAS(t.sent, "Authorization: Bearer sk-test");
    body = t.sent.substr(t.sent.find("\r\n\r\n") + 4);
    CHECK_EQ(jstr(body, "messages.0.role"), "system");
    CHECK(jnum(body, "max_completion_tokens") == 1024 && jget(body, "stream_options.include_usage").type == 't');
    rq.model = Config::builtinModels()[5];          // gemini
    rq.prov = 2;
    parseUrl("https://generativelanguage.googleapis.com", u);
    t.sent.clear();
    sendRequest(rq, u, t);
    HAS(t.sent, "POST /v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse HTTP/1.1");
    HAS(t.sent, "x-goog-api-key: sk-test");
    body = t.sent.substr(t.sent.find("\r\n\r\n") + 4);
    CHECK(jnum(body, "generationConfig.maxOutputTokens") == 1024);
    CHECK_EQ(jstr(body, "systemInstruction.parts.0.text"), "Be brief.");

    // errors
    std::string code, msg;
    httpError(401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", code, msg);
    CHECK(code == "KEY" && msg == "The provider rejected the API key (HTTP 401). invalid x-api-key");
    httpError(429, "{\"error\":{\"message\":\"slow\"}}", code, msg);
    CHECK(code == "LIMIT");
    httpError(529, "", code, msg);
    CHECK(code == "PROVIDER" && msg == "The provider is busy or down (HTTP 529); try again soon.");
    httpError(404, "[{\"error\":{\"message\":\"models/x is not found\"}}]", code, msg);
    CHECK(code == "MODEL" && msg.find("models/x is not found") != std::string::npos);
}

static void testHayes()
{
    Rig r;
    CHECK_EQ(r.mac_("AT\r"), "AT\r\r\nOK\r\n");
    CHECK_EQ(r.mac_("\rAT\r"), "\rAT\r\r\nOK\r\n");                         // Ask AI's wake-up
    CHECK_EQ(r.mac_("ATE0V1\r"), "ATE0V1\r\r\nOK\r\n");
    CHECK_EQ(r.mac_("AT\r"), "\r\nOK\r\n");
    CHECK_EQ(r.mac_("ATV0\r"), "0\r");
    CHECK_EQ(r.mac_("ATFOO\r"), "4\r");
    CHECK_EQ(r.mac_("ATV1 &C1 &D2 X4 S0=0 M0\r"), "\r\nOK\r\n");
    CHECK_EQ(r.mac_("ATS12?\r"), "\r\n050\r\n\r\nOK\r\n");
    CHECK_EQ(r.mac_("hello\r"), "");                                         // not a command: ignored
    CHECK_EQ(r.mac_("ATDT example.com:23\r"), "\r\nNO DIALTONE\r\n");        // no Wi-Fi
    CHECK_EQ(r.mac_("ATDT\r"), "\r\nERROR\r\n");
    HAS(r.mac_("ATI\r"), "Plus Modem 1.0");
    // keys only over USB; the echo never shows them
    std::string o = r.mac_("ATE1\rAT+KEY anthropic=sk-ant-secret-123456\r");
    LACKS(o, "secret");
    HAS(o, "AT+KEY anthropic=********************\r");
    HAS(o, "USB console");
    HAS(o, "ERROR");
    CHECK(r.cfg.keys[0].empty());
    for (size_t i = 0; i < r.dev.logs.size(); i++) LACKS(r.dev.logs[i], "secret");
    o = r.usb_("AT+KEY anthropic=sk-ant-secret-123456\r");
    LACKS(o, "secret");
    HAS(o, "anthropic key set: sk-...3456");
    CHECK(r.cfg.keys[0] == "sk-ant-secret-123456");
    o = r.usb_("AT+KEY\r");
    HAS(o, "sk-...3456");
    LACKS(o, "secret");
    // Wi-Fi from the Plus is fine, masked
    o = r.mac_("AT+WIFI=\"Home Net\",\"hunter22pw\"\r");
    LACKS(o, "hunter22pw");
    HAS(o, "OK");
    CHECK(r.cfg.nets.size() == 1 && r.cfg.nets[0].pass == "hunter22pw");
    o = r.usb_("AT+WIFIEAP=\"eduroam\",\"user@example.edu\",\"user@example.edu\",\"campuspw\"\r");
    LACKS(o, "campuspw");
    o = r.usb_("AT+WIFILIST\r");
    HAS(o, "1 Home Net: password set");
    HAS(o, "2 eduroam: WPA2-Enterprise, user user@example.edu");
    LACKS(o, "pw");
    HAS(r.usb_("AT+WIFIDEL=1\r"), "OK");
    CHECK(r.cfg.nets.size() == 1 && r.cfg.nets[0].ssid == "eduroam");
    // aliases, baud, flow, status
    HAS(r.usb_("AT+ALIAS mac=192.168.1.20:6801\r"), "OK");
    HAS(r.usb_("AT+ALIAS\r"), "mac = 192.168.1.20:6801");
    HAS(r.usb_("AT+ALIAS ai=1.2.3.4:5\r"), "ERROR");
    o = r.mac_("AT+BAUD=9600\r");
    CHECK_EQ(o, "AT+BAUD=9600\r\r\nOK\r\n");
    CHECK(r.dev.baud == 9600 && r.cfg.baud == 9600);
    HAS(r.usb_("AT+FLOW=XON\r"), "OK");
    CHECK(r.mac.xon);
    o = r.usb_("AT+STATUS\r");
    HAS(o, "Mac line: 9600 baud 8N1, flow XON");
    HAS(o, "Pins: RX=GPIO16");
    LACKS(o, "secret");
    HAS(r.mac_("AT+LOOP\r"), "ERROR");                                     // USB only
    HAS(r.usb_("AT+LOOP\r"), "LOOPBACK OK");
    // monitor lines on the USB console
    bool sawCmd = false;
    for (size_t i = 0; i < r.dev.logs.size(); i++) sawCmd |= r.dev.logs[i] == "[mac] AT+WIFI=\"Home Net\",************";
    CHECK(sawCmd);
}

static void testDataMode()
{
    Rig r;
    r.dev.wifi = true;
    CHECK_EQ(r.mac_("ATDT echo.test:23\r"), "ATDT echo.test:23\r\r\nCONNECT 19200\r\n");
    CHECK_EQ(r.mac_("hello+world", 100), "HELLO+WORLD");
    // "+++" needs a second of silence before and after
    CHECK_EQ(r.mac_("a+++", 1500), "A+++");                                 // no guard before: data
    r.run(1100);
    std::string o = r.mac_("+++", 1300);
    CHECK_EQ(o, "\r\nOK\r\n");
    CHECK_EQ(r.mac_("ATO\r"), "ATO\r\r\nCONNECT 19200\r\n");
    r.run(1100);
    CHECK_EQ(r.mac_("++", 1300), "++");                                     // two only: sent on as data
    CHECK_EQ(r.mac_("x"), "X");
    r.run(1100);
    r.mac_("+++", 1300);
    CHECK_EQ(r.mac_("ATH\r"), "ATH\r\r\nOK\r\n");
    CHECK(r.dev.echoDeleted && !r.mac.remote());
    CHECK_EQ(r.mac_("ATDT nowhere.test\r"), "ATDT nowhere.test\r\r\nNO ANSWER\r\n");
    HAS(r.usb_("AT+ALIAS bbs=echo.test:23\r"), "OK");
    CHECK_EQ(r.mac_("ATDTbbs\r"), "ATDTbbs\r\r\nCONNECT 19200\r\n");
    // XON/XOFF pacing toward the Mac
    r.mac.xon = true;
    r.mac_("\x13", 20);
    CHECK_EQ(r.mac_("abc"), "");
    CHECK_EQ(r.mac_("\x11"), "ABC");
    // slow Mac: the ring holds the bytes
    r.macPort.roomv = 0;
    r.mac_("xyz");
    r.macPort.roomv = 1 << 20;
    CHECK_EQ(r.mac_(""), "XYZ");
}

static void testBanner()
{
    Rig r;
    r.dev.now = 0;
    r.run(5000);
    CHECK_EQ(r.macPort.take(), "PLUS MODEM READY\r\nPLUS MODEM READY\r\nPLUS MODEM READY\r\n");
    r.mac_("\r", 10);
    r.run(5000);
    CHECK_EQ(r.macPort.take(), "");                                         // the first byte stops it
    Rig s;
    s.dev.now = 0;
    s.run(70000, 100);
    std::string all = s.macPort.take();
    size_t n = 0;
    for (size_t k = all.find("READY"); k != std::string::npos; k = all.find("READY", k + 1)) n++;
    CHECK(n == 30);                                                         // every 2 s for 60 s
}

static void testRelayMock()
{
    Rig r;
    std::string o = r.usb_("ATDT ai\r", 100);
    HAS(o, "CONNECT 115200\r\nASKAI 1 Plus Modem relay\r");
    o = relayExchange(r, "MODELS\r");
    HAS(o, "ERR PROTO Send HELLO first.\r");
    o = relayExchange(r, "HELLO 1 -\rMODELS\r");
    HAS(o, "OK HELLO ");
    HAS(o, " NEW 0 mock\r");
    HAS(o, "MODEL mock Mock Mock Model (free test)\rOK MODELS 1 mock\r");
    std::string sid = o.substr(o.find("OK HELLO ") + 9, 8);
    o = relayExchange(r, "SYSTEM Be brief.\rASK Hi there \xD2Mac\xD3\r", 6000);
    HAS(o, "OK SYSTEM\rBEGIN mock Mock\rT You said: ");
    HAS(plain(o), "You said: \"Hi there \xD2Mac\xD3\"\n\nHello from the mock provider! I'm a pretend model");
    HAS(o, "\rEND done ");
    LACKS(o, "**");
    o = relayExchange(r, "ASK what can a Plus do?\r", 8000);
    HAS(plain(o), "\n\xA5 Writing letters in MacWrite\n\xA5 Drawing in MacPaint\n");
    o = relayExchange(r, "ASK code\r", 8000);
    HAS(plain(o), "\n    int add(int a, int b)\n    {\n        return a + b;\n    }\n\nCode blocks");
    // STOP mid-reply, BUSY, PING
    o = relayExchange(r, "ASK !long\r", 900);
    HAS(o, "BEGIN mock Mock\r");
    o = relayExchange(r, "NEW\rPING\rSTOP\r", 100);
    HAS(o, "ERR BUSY");
    HAS(o, "PONG\r");
    HAS(o, "END stopped ");
    o = relayExchange(r, "ASK !error\r", 2000);
    HAS(o, "ERR PROVIDER The mock provider failed on purpose (you typed !error).\r");
    o = relayExchange(r, "ASK !refuse\r", 2000);
    HAS(o, "NOTE The model declined to answer that.\rEND refused 0 0\r");
    o = relayExchange(r, "ASK !cut\r", 3000);
    HAS(o, "END length ");
    o = relayExchange(r, "MORE line one\rASK line two\r", 4000);
    HAS(o, "OK MORE\rBEGIN");
    HAS(o, "You said: \"line one");
    o = relayExchange(r, "USE nothing\rFOO\r");
    HAS(o, "ERR MODEL No model called nothing here.\rERR PROTO Unknown command FOO.\r");
    o = relayExchange(r, "ASK " + std::string(2100, 'x') + "\r");
    HAS(o, "ERR TOOLONG");
    o = relayExchange(r, std::string(2100, 'y') + "\r");
    HAS(o, "ERR TOOLONG That line was too long.\r");
    // rate cap: 8 replies a minute (6 so far within this minute? count them all)
    int ok = 0, limited = 0;
    for (int i = 0; i < 9; i++) {
        o = relayExchange(r, "ASK !refuse\r", 700);
        ok += o.find("END refused") != std::string::npos;
        limited += o.find("ERR LIMIT Slow down") != std::string::npos;
    }
    CHECK(limited >= 1 && ok + limited == 9);
    // BYE hangs up; the session resumes
    o = relayExchange(r, "BYE\r", 200);
    HAS(o, "OK BYE\r");
    HAS(o, "NO CARRIER");
    CHECK(!r.usb.remote());
    r.run(61000);
    o = r.usb_("ATDT ai\r", 100);
    o = relayExchange(r, "HELLO 1 - " + sid + "\r");
    HAS(o, "OK HELLO " + sid + " RESUMED");
    // the line drops mid-session: ATH, then the Mac can dial again
    r.run(1100);
    r.usb_("+++", 1300);
    HAS(r.usb_("ATH\r"), "OK");
    CHECK(!r.modem.relay.busy());
}

static void testRelayPinAndBusy()
{
    Rig r;
    r.cfg.pin = "2468";
    r.usb_("ATDT ai\r");
    std::string o = relayExchange(r, "HELLO 1 1111\rHELLO 1 2222\r");
    HAS(o, "ERR PIN Wrong PIN.\rERR PIN Wrong PIN.\r");
    o = relayExchange(r, "HELLO 1 3333\r", 200);
    HAS(o, "BYE PIN Wrong PIN too many times.\r");
    HAS(o, "NO CARRIER");
    r.usb_("ATDT ai\r");
    o = relayExchange(r, "HELLO 1 2468\r");
    HAS(o, "OK HELLO");
    CHECK_EQ(r.mac_("ATDT ai\r"), "ATDT ai\r\r\nBUSY\r\n");                  // one relay connection at a time
    // no HELLO within 2 minutes
    Rig s;
    s.usb_("ATDT ai\r");
    o = relayExchange(s, "", 121000);
    HAS(o, "BYE IDLE No HELLO received.\r");
    HAS(o, "NO CARRIER");
}

static void testRelayHttp()
{
    Rig r;
    r.dev.wifi = true;
    r.usb_("AT+KEY anthropic=sk-test-0123456789\r");
    r.usb_("ATDT ai\r");
    std::string o = relayExchange(r, "HELLO 1 -\r");
    HAS(o, " NEW 0 claude\r");
    r.dev.tr.respond(chunked(anthropicStream("end_turn"), 23), 7);
    o = relayExchange(r, "ASK Hello\r", 500);
    CHECK(r.dev.tr.host == "api.anthropic.com" && r.dev.tr.port == 443 && r.dev.tr.tls);
    HAS(o, "BEGIN claude Claude\rT Hello ");
    CHECK_EQ(plain(o), "Hello \xD2Mac\xD3 Plus!\n\n\xA5 one\n\xA5 two");
    HAS(o, "\rEND done 17 ");
    CHECK(r.dev.tr.closed);
    // the history goes with the next request
    r.dev.tr.respond(chunked(anthropicStream("refusal"), 50), 50);
    o = relayExchange(r, "ASK again\r", 500);
    HAS(o, "END refused 17 ");
    std::string body = r.dev.tr.sent.substr(r.dev.tr.sent.find("\r\n\r\n") + 4);
    CHECK_EQ(jstr(body, "messages.0.content"), "Hello");
    CHECK_EQ(jstr(body, "messages.1.role"), "assistant");
    CHECK_EQ(jstr(body, "messages.2.content"), "again");
    // HTTP errors and error events
    std::string eb = "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"bad key\"}}";
    r.dev.tr.respond(fmt("HTTP/1.1 401 Unauthorized\r\ncontent-type: application/json\r\ncontent-length: %u\r\n\r\n", (unsigned)eb.size()) + eb, 9);
    o = relayExchange(r, "ASK x\r", 300);
    HAS(o, "ERR KEY The provider rejected the API key (HTTP 401). bad key\r");
    r.dev.tr.respond(chunked(sse("error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"), 99), 99);
    o = relayExchange(r, "ASK x\r", 300);
    HAS(o, "ERR PROVIDER Anthropic is overloaded right now; try again soon.\r");
    r.dev.tr.failOpen = true;
    o = relayExchange(r, "ASK x\r", 300);
    HAS(o, "ERR NET Couldn't reach the provider: host not found\r");
    r.dev.tr.failOpen = false;
    r.dev.wifi = false;
    o = relayExchange(r, "ASK x\r", 300);
    HAS(o, "ERR NET The modem isn't on Wi-Fi.\r");
    r.dev.wifi = true;
    // OpenAI-compatible (a local server: no key) and Gemini
    r.run(1100);
    r.usb_("+++", 1300);
    HAS(r.usb_("AT+URL local=http://192.168.1.20:11434/v1\r"), "OK");
    HAS(r.usb_("AT+KEY gemini=AIza-test-123456\r"), "OK");
    HAS(r.usb_("ATO\r"), "CONNECT");
    o = relayExchange(r, "MODELS\r");
    HAS(o, "MODEL local Local Local model (Ollama)\r");
    HAS(o, "MODEL gemini Gemini Google Gemini 2.5 Flash\r");
    LACKS(o, "MODEL gpt ");
    std::string oa = sse(0, "{\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}") +
                     sse(0, "{\"choices\":[{\"delta\":{\"content\":\"Hi from \"}}]}") +
                     sse(0, "{\"choices\":[{\"delta\":{\"content\":\"Ollama\"},\"finish_reason\":\"length\"}]}") +
                     sse(0, "{\"choices\":[],\"usage\":{\"completion_tokens\":5}}") + sse(0, "[DONE]");
    r.dev.tr.respond("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n" + oa, 13);
    o = relayExchange(r, "USE local\rASK hi\r", 500);
    HAS(o, "OK USE local Local Local model (Ollama)\r");
    CHECK(r.dev.tr.host == "192.168.1.20" && r.dev.tr.port == 11434 && !r.dev.tr.tls);
    HAS(r.dev.tr.sent, "POST /v1/chat/completions HTTP/1.1\r\nHost: 192.168.1.20:11434\r\n");
    LACKS(r.dev.tr.sent, "Authorization");
    CHECK_EQ(plain(o), "Hi from Ollama");
    HAS(o, "\rEND length 5 ");
    std::string gm = sse(0, "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"thinking\",\"thought\":true},{\"text\":\"G'day\"}],\"role\":\"model\"}}]}") +
                     sse(0, "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" mate\"}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"candidatesTokenCount\":3}}");
    r.dev.tr.respond(chunked(gm, 40), 40);
    o = relayExchange(r, "USE gemini\rASK hi\r", 500);
    CHECK_EQ(plain(o), "G'day mate");
    HAS(o, "\rEND done 3 ");
    LACKS(o, "thinking");
}

int main()
{
    testHelpers();
    testJson();
    testHttpSse();
    testProviders();
    testHayes();
    testDataMode();
    testBanner();
    testRelayMock();
    testRelayPinAndBusy();
    testRelayHttp();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
