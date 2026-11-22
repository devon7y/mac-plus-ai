#include "modem.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

static const char *resultText(int code)
{
    switch (code) {
    case RES_OK: return "OK";
    case RES_CONNECT: return "CONNECT";
    case RES_NOCARRIER: return "NO CARRIER";
    case RES_NODIALTONE: return "NO DIALTONE";
    case RES_BUSY: return "BUSY";
    case RES_NOANSWER: return "NO ANSWER";
    default: return "ERROR";
    }
}

static int number(const std::string &s, size_t &i)        // digits at s[i] (none: 0)
{
    int v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    return v;
}

// ======================================================================== Line
Line::Line(Modem &m, Port &port, bool isUsb, size_t ringSize)
    : usb(isUsb), bytesIn(0), m_(m), port_(port), remote_(0), cmdMode_(true), infoStarted_(false), xoffed_(false),
      ring_(ringSize), head_(0), count_(0), plus_(0), lastData_(0), lastPlus_(0)
{
    profile();
}

void Line::profile()
{
    if (usb) echo = verbose = true, quiet = false;
    else echo = m_.cfg.echo, verbose = m_.cfg.verbose, quiet = m_.cfg.quiet;
    xon = !usb && m_.cfg.flowXon;
    memset(S, 0, sizeof S);
    S[2] = '+';
    S[3] = 13;
    S[4] = 10;
    S[5] = 8;
    S[12] = 50;
}

void Line::send(const std::string &s)
{
    port_.write((const uint8_t *)s.data(), (int)s.size());
}

void Line::info(const std::string &text)
{
    if (!infoStarted_) { send("\r\n"); infoStarted_ = true; }
    send(text + "\r\n");
}

void Line::result(int code)
{
    if (code == RES_NONE) return;
    std::string t = resultText(code);
    if (code == RES_CONNECT) t += fmt(" %u", (unsigned)m_.baudOf(*this));
    if (!usb) m_.log("[mac] " + t);
    if (quiet) return;
    send(verbose ? "\r\n" + t + "\r\n" : fmt("%d\r", code));
}

void Line::connect(Remote *r)
{
    remote_ = r;
    cmdMode_ = true;                                // online once CONNECT has been sent (execute)
    head_ = count_ = 0;
    plus_ = 0;
    xoffed_ = false;
}

void Line::drop()
{
    if (!remote_) return;
    remote_->hangup();
    remote_ = 0;
    cmdMode_ = true;
    head_ = count_ = 0;
    plus_ = 0;
    xoffed_ = false;
}

void Line::poll()
{
    uint32_t now = m_.dev.ms();
    uint8_t buf[256];
    std::string out;
    for (int rounds = 0; rounds < 8; rounds++) {
        int n = port_.read(buf, sizeof buf);
        if (n <= 0) break;
        bytesIn += n;
        for (int i = 0; i < n; i++) {
            if (remote_ && !cmdMode_) dataByte(buf[i], now, out);
            else commandByte(buf[i]);
        }
    }
    if (remote_ && !out.empty()) remote_->write((const uint8_t *)out.data(), (int)out.size());
    now = m_.dev.ms();
    if (plus_ && now - lastPlus_ >= guardMs()) {   // the guard time after "+++" has passed
        if (plus_ == 3 && remote_) {
            plus_ = 0;
            cmdMode_ = true;
            if (!usb) m_.log("[mac] +++ (command mode; still connected: ATO goes back, ATH hangs up)");
            result(RES_OK);
        } else {                                    // only one or two: they were data after all
            if (remote_) remote_->write((const uint8_t *)std::string(plus_, (char)S[2]).data(), plus_);
            plus_ = 0;
        }
    }
    pump();
}

void Line::dataByte(uint8_t c, uint32_t now, std::string &out)
{
    if (xon && (c == 0x11 || c == 0x13)) { xoffed_ = c == 0x13; return; }
    if (c == S[2] && plus_ < 3 && (plus_ ? now - lastPlus_ < guardMs() : now - lastData_ >= guardMs())) {
        plus_++;                                    // held back until we know whether it's the escape
        lastPlus_ = now;
        return;
    }
    if (plus_) { out.append(plus_, (char)S[2]); plus_ = 0; }
    out += (char)c;
    lastData_ = now;
}

void Line::commandByte(uint8_t c)
{
    if (xon && (c == 0x11 || c == 0x13)) return;
    if (c == '\r') {
        if (echo) send("\r");
        std::string l;
        l.swap(cmd_);
        infoStarted_ = false;
        execute(l);
        return;
    }
    if (c == 8 || c == 127) {
        if (!cmd_.empty()) {
            cmd_.erase(cmd_.size() - 1);
            if (echo) send("\b \b");
        }
        return;
    }
    if (c < 32 || cmd_.size() >= 300) return;
    cmd_ += (char)c;
    if (echo) {
        int k = secretStart(cmd_);
        char e = k >= 0 && (int)cmd_.size() - 1 >= k ? '*' : (char)c;   // passwords and keys echo as *
        port_.write((const uint8_t *)&e, 1);
    }
}

void Line::execute(const std::string &line)
{
    std::string s = trim(line);
    if (s.size() < 2 || upper(s.substr(0, 2)) != "AT") return;   // not a command: ignored, as on a Hayes
    if (!usb) m_.log("[mac] " + maskLine(s));
    int res = RES_OK;
    size_t i = 2;
    while (i < s.size() && res == RES_OK) {
        char c = s[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        switch (c) {
        case ' ':
            break;
        case 'E': echo = number(s, i) != 0; break;
        case 'V': verbose = number(s, i) != 0; break;
        case 'Q': quiet = number(s, i) != 0; break;
        case 'Z': number(s, i); drop(); profile(); break;
        case 'H': if (number(s, i) == 0) drop(); break;
        case 'I':
            number(s, i);
            info(PM_NAME " " PM_VERSION ", an ESP32 Wi-Fi modem for the Macintosh Plus");
            info("AT+HELP lists the commands.");
            break;
        case 'O':
            number(s, i);
            if (!remote_) { result(RES_NOCARRIER); return; }
            result(RES_CONNECT);
            cmdMode_ = false;
            lastData_ = m_.dev.ms();
            return;
        case 'D': {
            int r = m_.dial(*this, s.substr(i));
            result(r);
            if (r == RES_CONNECT) { cmdMode_ = false; lastData_ = m_.dev.ms(); }
            return;
        }
        case 'S': {
            int reg = number(s, i);
            if (reg > 12) { res = RES_ERROR; break; }
            if (i < s.size() && s[i] == '=') { i++; S[reg] = (uint8_t)number(s, i); }
            else if (i < s.size() && s[i] == '?') { i++; info(fmt("%03u", S[reg])); }
            else res = RES_ERROR;
            break;
        }
        case '&': {
            char d = i < s.size() ? s[i++] : 0;
            if (d >= 'a' && d <= 'z') d -= 32;
            int v = number(s, i);
            if (d == 'F') {
                echo = verbose = true;
                quiet = false;
                S[2] = '+';
                S[12] = 50;
            } else if (d == 'W') {
                m_.cfg.echo = echo, m_.cfg.verbose = verbose, m_.cfg.quiet = quiet;
                m_.dev.save(m_.cfg);
            } else if (d == 'V') {
                info(fmt("E%d V%d Q%d S2=%u S12=%u", echo, verbose, quiet, S[2], S[12]) +
                     fmt("  baud %u, flow %s", (unsigned)m_.baudOf(*this), xon ? "XON" : "NONE"));
            } else if (d == 'K' && !usb && (v == 0 || v == 4)) {
                xon = m_.cfg.flowXon = v == 4;
            } else if (!d) {
                res = RES_ERROR;
            }                                       // &C &D &K &S ...: accepted, nothing to do
            break;
        }
        case '+':
            res = m_.extended(*this, s.substr(i));
            i = s.size();
            break;
        case 'B': case 'C': case 'L': case 'M': case 'N': case 'P': case 'T': case 'W': case 'X': case 'Y':
            number(s, i);                           // speaker, result-code level...: accepted
            break;
        default:
            res = RES_ERROR;
        }
    }
    result(res);
}

void Line::pump()
{
    if (!remote_) return;
    remote_->poll();
    uint8_t tmp[256];
    while (count_ < ring_.size()) {                 // the network side, into the ring
        int a = remote_->available();
        if (a <= 0) break;
        size_t k = ring_.size() - count_;
        if (k > sizeof tmp) k = sizeof tmp;
        if ((size_t)a < k) k = a;
        int n = remote_->read(tmp, (int)k);
        if (n <= 0) break;
        for (int j = 0; j < n; j++) ring_[(head_ + count_ + j) % ring_.size()] = tmp[j];
        count_ += n;
    }
    if (!cmdMode_ && !xoffed_) {                    // the ring, out to the port as fast as it takes it
        int room = port_.room();
        while (room > 0 && count_ > 0) {
            size_t k = ring_.size() - head_;
            if (k > count_) k = count_;
            if (k > (size_t)room) k = room;
            port_.write(&ring_[head_], (int)k);
            head_ = (head_ + k) % ring_.size();
            count_ -= k;
            room -= (int)k;
        }
    }
    if (!remote_->alive() && (cmdMode_ || count_ == 0)) {
        drop();
        result(RES_NOCARRIER);
    }
}

// ======================================================================== Modem
Modem::Modem(Config &c, Device &d) : cfg(c), dev(d), relay(c, d), mac_(0), usb_(0), nextBanner_(0) {}

void Modem::log(const std::string &line)
{
    if (line.compare(0, 5, "[mac]") == 0 && !cfg.monitor) return;
    if (usb_ && usb_->online()) return;             // never into the USB console's data stream
    dev.log(line);
}

void Modem::poll()
{
    // For the first minute, tell the Mac we're here until it sends anything (HARDWARE.md step 7).
    uint32_t now = dev.ms();
    if (mac_ && now < 60000 && mac_->bytesIn == 0 && mac_->idle() && (int32_t)(now - nextBanner_) >= 0) {
        nextBanner_ = now + 2000;
        mac_->send("PLUS MODEM READY\r\n");
    }
    if (mac_) mac_->poll();
    if (usb_) usb_->poll();
}

int Modem::dial(Line &l, const std::string &rest)
{
    std::string d = trim(rest);
    if (!d.empty() && strchr("TtPp", d[0])) d = trim(d.substr(1));
    if (d.size() >= 2 && d[0] == '"') d = d.substr(1, d.find('"', 1) == std::string::npos ? std::string::npos : d.find('"', 1) - 1);
    d = trim(d);
    if (d.empty()) return RES_ERROR;
    l.drop();                                       // a connection still held: hang it up first
    if (lower(d) == "ai") {
        if (!relay.open()) return RES_BUSY;
        l.connect(&relay);
        log(std::string(l.usb ? "[usb]" : "[mac]") + " connected to the AI relay");
        return RES_CONNECT;
    }
    std::string target = d;
    for (size_t i = 0; i < cfg.aliases.size(); i++)
        if (lower(cfg.aliases[i].name) == lower(d)) target = cfg.aliases[i].target;
    std::string host;
    uint16_t port;
    if (!splitHostPort(target, host, port, 23)) return RES_ERROR;
    if (!dev.wifiUp()) return RES_NODIALTONE;
    log(fmt("%s dialling %s:%u", l.usb ? "[usb]" : "[mac]", host.c_str(), port));
    Remote *r = dev.dialTcp(host, port);
    if (!r) return RES_NOANSWER;
    l.connect(r);
    return RES_CONNECT;
}

void Modem::help(Line &l)
{
    static const char *const H[] = {
        PM_NAME " " PM_VERSION " commands (type AT first):",
        " DT host:port  dial; DT ai = AI relay; DT alias",
        " H hang up   O back online   Z reset   I info",
        " E0/1 echo   V0/1 words   Q0/1 quiet   &F &W &V",
        " +++ with 1 s pauses: command mode, still online",
        "+STATUS  Wi-Fi, line, pins, memory",
        "+SCAN    Wi-Fi networks nearby",
        "+WIFI=\"name\",\"password\"   add a network",
        "+WIFIEAP=\"eduroam\",\"id\",\"user\",\"password\"",
        "+WIFILIST   +WIFIDEL=n",
        "+BAUD=19200   Mac speed, 1200-57600",
        "+FLOW=NONE or XON   XON/XOFF from the Mac",
        "+ALIAS name=host:port   (+ALIAS lists)",
        "+MODEL   models for DT ai",
        "+TLS=host   HTTPS test   +MON=0/1 USB monitor",
        "USB console only: +KEY +URL +PIN +MODEL= +LOOP",
    };
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) l.info(H[i]);
}

void Modem::status(Line &l)
{
    uint32_t s = dev.ms() / 1000;
    l.info(fmt(PM_NAME " " PM_VERSION ", up %ud %02u:%02u:%02u", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60));
    l.info("Wi-Fi: " + dev.wifiStatus());
    l.info(fmt("Mac line: %u baud 8N1, flow %s", (unsigned)cfg.baud, cfg.flowXon ? "XON" : "NONE"));
    l.info("Pins: " + dev.pinInfo());
    Line *ls[2] = {mac_, usb_};
    for (int i = 0; i < 2; i++) {
        if (!ls[i]) continue;
        Remote *r = ls[i]->remote();
        l.info(std::string(i ? "USB: " : "Mac: ") + (!r ? "idle" : (ls[i]->online() ? "online, " : "command mode, holding ") + r->describe()));
    }
    std::string k;
    for (int p = 0; p < NPROV; p++)
        if (PROVIDERS[p].needsKey) k += std::string(k.empty() ? "" : ", ") + PROVIDERS[p].name + " " + maskKey(cfg.keys[p]);
    l.info("Keys: " + k);
    const ModelDef *m = cfg.defaultUsable();
    l.info("AI default model: " + (m ? m->id + " (" + m->label + ")" : std::string("none")) + (cfg.pin.empty() ? "" : ", PIN set"));
    l.info(fmt("Memory: %u KB free, lowest %u KB", (unsigned)(dev.freeHeap() / 1024), (unsigned)(dev.minFreeHeap() / 1024)));
}

int Modem::wifi(Line &l, const std::string &args, bool eap)
{
    std::vector<std::string> a;
    if (!splitArgs(args, a)) { l.info("Check the quotes."); return RES_ERROR; }
    WifiNet n;
    n.eap = eap;
    if (!eap && (a.size() == 1 || a.size() == 2)) {
        n.ssid = a[0];
        n.pass = a.size() == 2 ? a[1] : "";
        if (!n.pass.empty() && (n.pass.size() < 8 || n.pass.size() > 64)) { l.info("A Wi-Fi password has 8 to 63 characters."); return RES_ERROR; }
    } else if (eap && (a.size() == 3 || a.size() == 4)) {
        n.ssid = a[0];
        n.ident = a[1];
        n.user = a.size() == 4 ? a[2] : a[1];
        n.pass = a.back();
    } else {
        l.info(eap ? "Use AT+WIFIEAP=\"ssid\",\"identity\",\"username\",\"password\"" : "Use AT+WIFI=\"ssid\",\"password\"");
        return RES_ERROR;
    }
    if (n.ssid.empty() || n.ssid.size() > 32) { l.info("The network name has 1 to 32 characters."); return RES_ERROR; }
    size_t k = 0;
    while (k < cfg.nets.size() && cfg.nets[k].ssid != n.ssid) k++;
    if (k == cfg.nets.size()) {
        if (k >= MAX_NETS) { l.info("The list is full: delete one with AT+WIFIDEL=n."); return RES_ERROR; }
        cfg.nets.push_back(n);
    } else {
        cfg.nets[k] = n;
    }
    dev.save(cfg);
    dev.wifiChanged();
    l.info("Saved " + n.ssid + "; joining the strongest known network now.");
    return RES_OK;
}

int Modem::alias(Line &l, const std::string &args)
{
    std::string a = trim(args);
    if (a.empty() || a == "?") {
        if (cfg.aliases.empty()) l.info("No aliases. Add one: AT+ALIAS name=host:port");
        for (size_t i = 0; i < cfg.aliases.size(); i++) l.info(cfg.aliases[i].name + " = " + cfg.aliases[i].target);
        return RES_OK;
    }
    size_t eq = a.find('=');
    std::string name = trim(a.substr(0, eq)), target = eq == std::string::npos ? "" : trim(a.substr(eq + 1));
    if (eq == std::string::npos || name.empty() || name.size() > 31 || name.find(' ') != std::string::npos || lower(name) == "ai") {
        l.info("Use AT+ALIAS name=host:port (the name ai is taken).");
        return RES_ERROR;
    }
    size_t k = 0;
    while (k < cfg.aliases.size() && lower(cfg.aliases[k].name) != lower(name)) k++;
    if (target.empty()) {                           // name= deletes it
        if (k == cfg.aliases.size()) return RES_ERROR;
        cfg.aliases.erase(cfg.aliases.begin() + k);
    } else {
        std::string host;
        uint16_t port;
        if (!splitHostPort(target, host, port, 23)) { l.info("Use host:port, e.g. 192.168.1.20:6801"); return RES_ERROR; }
        Alias al = {name, host + fmt(":%u", port)};
        if (k < cfg.aliases.size()) cfg.aliases[k] = al;
        else if (k >= MAX_ALIASES) { l.info("Too many aliases."); return RES_ERROR; }
        else cfg.aliases.push_back(al);
    }
    dev.save(cfg);
    return RES_OK;
}

int Modem::key(Line &l, const std::string &args, bool url)
{
    std::string a = trim(args);
    if (a.empty() || a == "?") {
        for (int p = 0; p < NPROV; p++) {
            if (PROVIDERS[p].type == P_MOCK) continue;
            std::string u = cfg.url(p);
            l.info(fmt("%-10s ", PROVIDERS[p].name) + (PROVIDERS[p].needsKey || !cfg.keys[p].empty() ? fmt("%-12s ", maskKey(cfg.keys[p]).c_str()) : std::string(13, ' ')) +
                   (u.empty() ? "(no address: AT+URL)" : u));
        }
        return RES_OK;
    }
    if (!l.usb) { l.info("Keys and addresses are set from the USB console (the MacBook) only."); return RES_ERROR; }
    size_t eq = a.find('=');
    int p = providerIndex(trim(a.substr(0, eq)));
    if (eq == std::string::npos || p < 0 || PROVIDERS[p].type == P_MOCK) {
        l.info(url ? "Use AT+URL provider=https://..." : "Use AT+KEY provider=key");
        l.info("Providers: anthropic openai gemini openrouter local");
        return RES_ERROR;
    }
    std::string v = trim(a.substr(eq + 1));
    if (url) {
        Url u;
        if (!v.empty() && !parseUrl(v, u)) { l.info("That isn't an http:// or https:// address."); return RES_ERROR; }
        cfg.urls[p] = v;
        l.info(std::string(PROVIDERS[p].name) + " address: " + (cfg.url(p).empty() ? "none" : cfg.url(p)));
    } else {
        cfg.keys[p] = v;
        l.info(std::string(PROVIDERS[p].name) + " key " + (v.empty() ? "cleared" : "set: " + maskKey(v)));
    }
    dev.save(cfg);
    return RES_OK;
}

int Modem::model(Line &l, const std::string &rest)
{
    std::string r = trim(rest);
    if (r.empty() || r == "?") {
        const ModelDef *d = cfg.defaultUsable();
        for (size_t i = 0; i < cfg.models.size(); i++) {
            const ModelDef &m = cfg.models[i];
            l.info(fmt("%c %-14s ", d && d->id == m.id ? '*' : ' ', m.id.c_str()) + m.provider + "/" + m.model +
                   (cfg.usable(m) ? "" : providerIndex(m.provider) >= 0 && cfg.url(providerIndex(m.provider)).empty() ? " (no address)" : " (no key)"));
        }
        l.info("* = default. Usable models appear in Ask AI's Model menu.");
        return RES_OK;
    }
    if (!l.usb) { l.info("Models are changed from the USB console (the MacBook) only."); return RES_ERROR; }
    if (r[0] == '=') {                              // AT+MODEL=id: the default model
        std::string id = trim(r.substr(1));
        bool known = false;
        for (size_t i = 0; i < cfg.models.size(); i++) known |= cfg.models[i].id == id;
        if (!known) { l.info("No model " + id + " in the list (AT+MODEL lists them)."); return RES_ERROR; }
        cfg.defaultModel = id;
    } else if (upper(r) == "RESET") {
        cfg.models = Config::builtinModels();
        cfg.modelsEdited = false;
        cfg.defaultModel.clear();
    } else {                                        // AT+MODEL id=provider/model,label,tag (id= deletes)
        size_t eq = r.find('=');
        std::string id = trim(r.substr(0, eq)), def = eq == std::string::npos ? "" : trim(r.substr(eq + 1));
        if (eq == std::string::npos || id.empty() || id.size() > 31 || id.find(' ') != std::string::npos) {
            l.info("Use AT+MODEL id=provider/model,label,tag  (id= deletes; AT+MODEL=id sets the default)");
            return RES_ERROR;
        }
        size_t k = 0;
        while (k < cfg.models.size() && cfg.models[k].id != id) k++;
        if (def.empty()) {
            if (k == cfg.models.size()) return RES_ERROR;
            cfg.models.erase(cfg.models.begin() + k);
        } else {
            std::vector<std::string> a;
            splitArgs(def, a);
            size_t slash = a.empty() ? std::string::npos : a[0].find('/');
            if (slash == std::string::npos || providerIndex(a[0].substr(0, slash)) < 0 || slash + 1 >= a[0].size()) {
                l.info("Providers: anthropic openai gemini openrouter local mock");
                return RES_ERROR;
            }
            ModelDef m;
            m.id = id;
            m.provider = lower(a[0].substr(0, slash));
            m.model = a[0].substr(slash + 1);
            m.label = a.size() > 1 && !a[1].empty() ? a[1].substr(0, 40) : id;
            std::string tag = a.size() > 2 && !a[2].empty() ? a[2] : "AI";
            tag.erase(std::remove(tag.begin(), tag.end(), ' '), tag.end());
            m.tag = tag.substr(0, 8);
            if (k < cfg.models.size()) cfg.models[k] = m;
            else if (k >= MAX_MODELS) { l.info("Too many models."); return RES_ERROR; }
            else cfg.models.push_back(m);
        }
        cfg.modelsEdited = true;
    }
    dev.save(cfg);
    return RES_OK;
}

int Modem::extended(Line &l, const std::string &cmd)
{
    std::string c = trim(cmd);
    size_t k = 0;
    while (k < c.size() && isalpha((unsigned char)c[k])) k++;
    std::string name = upper(c.substr(0, k)), rest = c.substr(k), args = rest;
    if (!args.empty() && (args[0] == '=' || args[0] == ' ')) args = args.substr(1);
    if (args == "?") args.clear();

    if (name == "HELP") { help(l); return RES_OK; }
    if (name == "STATUS") { status(l); return RES_OK; }
    if (name == "SCAN") {
        std::vector<ScanEntry> v;
        if (!dev.scan(v)) { l.info("The scan failed."); return RES_ERROR; }
        if (v.empty()) l.info("No networks found (2.4 GHz only).");
        for (size_t i = 0; i < v.size() && i < 20; i++) {
            bool known = false;
            for (size_t j = 0; j < cfg.nets.size(); j++) known |= cfg.nets[j].ssid == v[i].ssid;
            l.info(fmt("%4d dBm ch%-2d %-5s ", v[i].rssi, v[i].channel, v[i].open ? "open" : v[i].eap ? "EAP" : "WPA") + v[i].ssid + (known ? "  (known)" : ""));
        }
        return RES_OK;
    }
    if (name == "WIFI" && !args.empty()) return wifi(l, args, false);
    if (name == "WIFIEAP") return wifi(l, args, true);
    if (name == "WIFI" || name == "WIFILIST") {
        if (cfg.nets.empty()) l.info("No networks. Add one: AT+WIFI=\"name\",\"password\"");
        for (size_t i = 0; i < cfg.nets.size(); i++) {
            const WifiNet &n = cfg.nets[i];
            l.info(fmt("%u ", (unsigned)i + 1) + n.ssid + ": " + (n.eap ? "WPA2-Enterprise, user " + n.user : n.pass.empty() ? "open" : "password set"));
        }
        return RES_OK;
    }
    if (name == "WIFIDEL") {
        int n = atoi(args.c_str());
        if (n < 1 || n > (int)cfg.nets.size()) return RES_ERROR;
        cfg.nets.erase(cfg.nets.begin() + (n - 1));
        dev.save(cfg);
        dev.wifiChanged();
        return RES_OK;
    }
    if (name == "BAUD") {
        if (args.empty()) { l.info(fmt("%u", (unsigned)cfg.baud)); return RES_OK; }
        long b = atol(args.c_str());
        if (b < 1200 || b > 57600) { l.info("1200 to 57600"); return RES_ERROR; }
        cfg.baud = (uint32_t)b;
        dev.save(cfg);
        if (&l == mac_) {                           // answer at the old speed, then switch
            l.result(RES_OK);
            l.port().drain();
            dev.setBaud(cfg.baud);
            return RES_NONE;
        }
        dev.setBaud(cfg.baud);
        return RES_OK;
    }
    if (name == "FLOW") {
        std::string a = upper(trim(args));
        if (a.empty()) { l.info(cfg.flowXon ? "XON" : "NONE"); return RES_OK; }
        if (a != "NONE" && a != "XON" && a != "XONXOFF" && a != "XON/XOFF") return RES_ERROR;
        cfg.flowXon = a != "NONE";
        if (mac_) mac_->xon = cfg.flowXon;
        dev.save(cfg);
        return RES_OK;
    }
    if (name == "ALIAS") return alias(l, args);
    if (name == "KEY") return key(l, args, false);
    if (name == "URL") return key(l, args, true);
    if (name == "MODEL") return model(l, rest == "?" ? "" : rest);
    if (name == "PIN") {
        if (args.empty() && rest.find('=') == std::string::npos) { l.info(cfg.pin.empty() ? "No PIN." : "A PIN is set."); return RES_OK; }
        if (!l.usb) { l.info("The PIN is set from the USB console (the MacBook) only."); return RES_ERROR; }
        std::string p = trim(args);
        if (p.find(' ') != std::string::npos || p.size() > 31) return RES_ERROR;
        cfg.pin = p;
        dev.save(cfg);
        return RES_OK;
    }
    if (name == "MON") {
        if (args.empty()) { l.info(cfg.monitor ? "1" : "0"); return RES_OK; }
        cfg.monitor = atoi(args.c_str()) != 0;
        dev.save(cfg);
        return RES_OK;
    }
    if (name == "LOOP") {
        if (!l.usb) { l.info("Run AT+LOOP from the USB console, with DB9 pins 2 and 3 joined."); return RES_ERROR; }
        std::vector<std::string> out;
        bool ok = dev.loopback(out);
        for (size_t i = 0; i < out.size(); i++) l.info(out[i]);
        return ok ? RES_OK : RES_ERROR;
    }
    if (name == "TLS") {
        if (!dev.wifiUp()) { l.info("Not on Wi-Fi."); return RES_NODIALTONE; }
        std::vector<std::string> out;
        bool ok = dev.tlsTest(args.empty() ? "api.anthropic.com" : trim(args), out);
        for (size_t i = 0; i < out.size(); i++) l.info(out[i]);
        return ok ? RES_OK : RES_ERROR;
    }
    return RES_ERROR;
}
