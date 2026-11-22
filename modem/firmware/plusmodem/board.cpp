// Plus Modem: the ESP32 side. UART ports, pin detection, Wi-Fi, settings in flash, TCP and TLS
// sockets, and the self-tests. Everything else is the portable core (modem.cpp, relay.cpp, ...).
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wpa2.h>
#include <algorithm>
#include "ca_bundle.h"
#include "modem.h"

static const int PIN_A = 16, PIN_B = 17;          // UART2's usual pins (RX2, TX2)
static const size_t MAC_RX_BUF = 4096;

// ------------------------------------------------------------------------ serial ports
class UartPort : public Port {
public:
    explicit UartPort(HardwareSerial &s) : s_(s) {}
    int read(uint8_t *b, int max)
    {
        int n = s_.available();
        if (n <= 0) return 0;
        return (int)s_.read(b, std::min(n, max));
    }
    int room() { return s_.availableForWrite(); }
    void write(const uint8_t *p, int n) { s_.write(p, n); }
    void drain() { s_.flush(); }
private:
    HardwareSerial &s_;
};

static Config cfg;
static UartPort usbPort(Serial), macPort(Serial2);
static Modem *modem;
static Line *macLine, *usbLine;

// ------------------------------------------------------------------------ pins
// The MAX3232's TTL output idles high; the ESP32's pull-downs keep the module's input low. So the
// pin that reads high is RX. The two data jumpers can be wired either way round.
static int rxPin = PIN_A, txPin = PIN_B;
static bool pinsFound;
static std::string pinNote;

static void detectPins()
{
    pinMode(PIN_A, INPUT_PULLDOWN);
    pinMode(PIN_B, INPUT_PULLDOWN);
    delay(20);
    int a = 0, b = 0;
    for (int i = 0; i < 20; i++) {
        a += digitalRead(PIN_A);
        b += digitalRead(PIN_B);
        delay(1);
    }
    bool ha = a >= 18, hb = b >= 18;
    pinsFound = ha != hb;
    rxPin = hb && !ha ? PIN_B : PIN_A;
    txPin = rxPin == PIN_A ? PIN_B : PIN_A;
    pinNote = fmt("RX=GPIO%d TX=GPIO%d", rxPin, txPin);
    if (pinsFound) pinNote += rxPin == PIN_A ? " (detected)" : " (detected; the data wires are swapped, which is fine)";
    else if (ha) pinNote += " (default: both pins read high)";
    else pinNote += " (default: both pins read low; is the MAX3232 module connected?)";
}

static void macBegin(uint32_t baud)
{
    Serial2.setRxBufferSize(MAC_RX_BUF);
    Serial2.begin(baud, SERIAL_8N1, rxPin, txPin);
}

// ------------------------------------------------------------------------ settings in flash
static Preferences prefs;

static void putStr(const char *key, const std::string &v)
{
    if (prefs.getString(key, "") != v.c_str()) prefs.putString(key, v.c_str());
}

static void saveConfig(const Config &c)
{
    prefs.begin("plusmodem", false);
    prefs.putUInt("baud", c.baud);
    prefs.putUChar("flow", c.flowXon);
    prefs.putUChar("echo", c.echo);
    prefs.putUChar("verb", c.verbose);
    prefs.putUChar("quiet", c.quiet);
    prefs.putUChar("mon", c.monitor);
    putStr("nets", c.netsText());
    putStr("alias", c.aliasesText());
    if (c.modelsEdited) putStr("models", c.modelsText());
    else if (prefs.isKey("models")) prefs.remove("models");
    putStr("defmodel", c.defaultModel);
    putStr("pin", c.pin);
    for (int p = 0; p < NPROV; p++) {
        putStr((std::string("k_") + PROVIDERS[p].name).c_str(), c.keys[p]);
        putStr((std::string("u_") + PROVIDERS[p].name).c_str(), c.urls[p]);
    }
    prefs.end();
}

static std::string getStr(const char *key) { return prefs.getString(key, "").c_str(); }

static void loadConfig(Config &c)
{
    c.defaults();
    if (!prefs.begin("plusmodem", true)) return;    // nothing saved yet
    c.baud = prefs.getUInt("baud", c.baud);
    if (c.baud < 1200 || c.baud > 57600) c.baud = 19200;
    c.flowXon = prefs.getUChar("flow", c.flowXon);
    c.echo = prefs.getUChar("echo", c.echo);
    c.verbose = prefs.getUChar("verb", c.verbose);
    c.quiet = prefs.getUChar("quiet", c.quiet);
    c.monitor = prefs.getUChar("mon", c.monitor);
    c.parseNets(getStr("nets"));
    c.parseAliases(getStr("alias"));
    if (prefs.isKey("models")) c.parseModels(getStr("models"));
    c.defaultModel = getStr("defmodel");
    c.pin = getStr("pin");
    for (int p = 0; p < NPROV; p++) {
        c.keys[p] = getStr((std::string("k_") + PROVIDERS[p].name).c_str());
        c.urls[p] = getStr((std::string("u_") + PROVIDERS[p].name).c_str());
    }
    prefs.end();
}

// ------------------------------------------------------------------------ Wi-Fi
// Scan, join the strongest known network, rejoin when it drops.
enum { W_IDLE, W_SCAN, W_JOIN, W_UP, W_WAIT };
static int wstate = W_IDLE;
static uint32_t wdeadline;
static std::vector<int> cands;                     // indexes into cfg.nets, strongest first
static size_t cand;
static std::string wnote = "starting";

static void wlog(const std::string &s)
{
    wnote = s;
    if (modem) modem->log("[wifi] " + s);
}

static void join(size_t k)
{
    const WifiNet &n = cfg.nets[cands[k]];
    cand = k;
    WiFi.disconnect();
    if (n.eap) {
        WiFi.begin(n.ssid.c_str(), WPA2_AUTH_PEAP, n.ident.c_str(), n.user.c_str(), n.pass.c_str());
    } else {
        esp_wifi_sta_wpa2_ent_disable();           // a plain WiFi.begin() leaves enterprise mode on
        WiFi.begin(n.ssid.c_str(), n.pass.empty() ? NULL : n.pass.c_str());
    }
    wstate = W_JOIN;
    wdeadline = millis() + (n.eap ? 30000 : 20000);
    wlog("joining " + n.ssid + (n.eap ? " (WPA2-Enterprise)" : ""));
}

static void wifiPoll()
{
    switch (wstate) {
    case W_IDLE:
        if (cfg.nets.empty()) { wnote = "no networks set: AT+WIFI=\"name\",\"password\""; return; }
        WiFi.scanNetworks(true);
        wstate = W_SCAN;
        wdeadline = millis() + 15000;
        break;
    case W_SCAN: {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING && (int32_t)(millis() - wdeadline) < 0) return;
        cands.clear();
        std::vector<int> rssi;
        for (size_t k = 0; k < cfg.nets.size(); k++) {
            int best = -1000;
            for (int i = 0; i < n; i++)
                if (WiFi.SSID(i) == cfg.nets[k].ssid.c_str()) best = std::max(best, (int)WiFi.RSSI(i));
            if (best > -1000) { cands.push_back((int)k); rssi.push_back(best); }
        }
        WiFi.scanDelete();
        for (size_t i = 0; i < cands.size(); i++)  // strongest first
            for (size_t j = i + 1; j < cands.size(); j++)
                if (rssi[j] > rssi[i]) { std::swap(rssi[i], rssi[j]); std::swap(cands[i], cands[j]); }
        if (cands.empty()) {
            wnote = fmt("not connected: none of the %u known networks is in range", (unsigned)cfg.nets.size());
            wstate = W_WAIT;
            wdeadline = millis() + 15000;
        } else {
            join(0);
        }
        break;
    }
    case W_JOIN:
        if (WiFi.status() == WL_CONNECTED) {
            wstate = W_UP;
            wlog("connected to " + cfg.nets[cands[cand]].ssid + ", IP " + WiFi.localIP().toString().c_str());
        } else if ((int32_t)(millis() - wdeadline) >= 0) {
            wlog("couldn't join " + cfg.nets[cands[cand]].ssid + " (wrong password?)");
            if (cand + 1 < cands.size()) join(cand + 1);
            else { WiFi.disconnect(); wstate = W_WAIT; wdeadline = millis() + 10000; }
        }
        break;
    case W_UP:
        if (WiFi.status() != WL_CONNECTED) {
            wlog("lost the connection; reconnecting");
            wstate = W_WAIT;
            wdeadline = millis() + 2000;
        }
        break;
    case W_WAIT:
        if ((int32_t)(millis() - wdeadline) >= 0) wstate = W_IDLE;
        break;
    }
}

// ------------------------------------------------------------------------ sockets
class TcpRemote : public Remote {
public:
    TcpRemote(const WiFiClient &c, const std::string &d) : c_(c), desc_(d) { c_.setNoDelay(true); }
    bool alive() { return c_.connected() || c_.available() > 0; }
    int available() { return c_.available(); }
    int read(uint8_t *b, int max) { return c_.read(b, max); }
    int write(const uint8_t *p, int n) { return (int)c_.write(p, n); }
    void hangup() { c_.stop(); delete this; }
    std::string describe() { return "TCP " + desc_; }
private:
    WiFiClient c_;
    std::string desc_;
};

static WiFiClientSecure tlsClient;                 // the CA bundle is attached once, in setup()

class NetTransport : public Transport {
public:
    NetTransport() : c_(0) {}
    bool open(const std::string &host, uint16_t port, bool tls, std::string &err)
    {
        close();
        if (tls) {
            tlsClient.setHandshakeTimeout(20);
            if (!tlsClient.connect(host.c_str(), port)) {
                char e[120] = "";
                tlsClient.lastError(e, sizeof e);
                err = e[0] ? e : "TLS connection failed";
                return false;
            }
            c_ = &tlsClient;
        } else {
            if (!plain_.connect(host.c_str(), port, 10000)) { err = "connection refused or timed out"; return false; }
            c_ = &plain_;
        }
        return true;
    }
    bool send(const char *p, size_t n)
    {
        for (size_t done = 0; done < n;) {
            size_t k = c_ ? c_->write((const uint8_t *)p + done, n - done) : 0;
            if (k == 0) return false;
            done += k;
        }
        return true;
    }
    int recv(uint8_t *b, size_t max)
    {
        if (!c_) return -1;
        int a = c_->available();
        if (a > 0) return c_->read(b, std::min((size_t)a, max));
        return c_->connected() ? 0 : -1;
    }
    void close()
    {
        if (c_) c_->stop();
        c_ = 0;
    }
    bool busy() const { return c_ != 0; }
private:
    Client *c_;
    WiFiClient plain_;
};

static NetTransport transport;

// ------------------------------------------------------------------------ the Device
class EspDevice : public Device {
public:
    uint32_t ms() { return millis(); }
    uint32_t random32() { return esp_random(); }
    void log(const std::string &line) { Serial.print((line + "\r\n").c_str()); }
    bool wifiUp() { return wstate == W_UP && WiFi.status() == WL_CONNECTED; }
    std::string wifiStatus()
    {
        if (wifiUp())
            return "connected to " + cfg.nets[cands[cand]].ssid + fmt(" (%d dBm), IP ", (int)WiFi.RSSI()) + WiFi.localIP().toString().c_str();
        return wnote;
    }
    void wifiChanged()
    {
        WiFi.disconnect();
        wstate = W_IDLE;
    }
    bool scan(std::vector<ScanEntry> &out)
    {
        for (uint32_t t = millis(); WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t < 10000;) delay(50);
        int n = WiFi.scanNetworks(false, true);
        if (n < 0) return false;
        for (int i = 0; i < n; i++) {
            ScanEntry e = {WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                           WiFi.encryptionType(i) == WIFI_AUTH_OPEN, WiFi.encryptionType(i) == WIFI_AUTH_WPA2_ENTERPRISE};
            out.push_back(e);
        }
        WiFi.scanDelete();
        std::sort(out.begin(), out.end(), [](const ScanEntry &a, const ScanEntry &b) { return a.rssi > b.rssi; });
        if (wstate == W_SCAN) wstate = W_IDLE;      // our scan replaced the background one
        return true;
    }
    Remote *dialTcp(const std::string &host, uint16_t port)
    {
        WiFiClient c;
        if (!c.connect(host.c_str(), port, 10000)) return 0;
        return new TcpRemote(c, fmt("%s:%u", host.c_str(), port));
    }
    Transport *transport() { return &::transport; }
    void save(const Config &c) { saveConfig(c); }
    void setBaud(uint32_t baud) { Serial2.updateBaudRate(baud); }
    std::string pinInfo() { return pinNote; }

    bool loopback(std::vector<std::string> &out)
    {
        Serial2.end();
        detectPins();
        out.push_back("Pins: " + pinNote);
        uint32_t speeds[2] = {cfg.baud, cfg.baud == 57600 ? 9600u : 57600u};
        bool ok = true;
        for (int s = 0; s < 2; s++) {
            macBegin(speeds[s]);
            delay(20);
            while (Serial2.available()) Serial2.read();
            uint8_t pat[256], got[256];
            for (int i = 0; i < 256; i++) pat[i] = (uint8_t)i;
            Serial2.write(pat, sizeof pat);
            size_t n = 0;
            uint32_t t0 = millis(), limit = 256 * 10 * 1000 / speeds[s] + 300;
            while (n < sizeof got && millis() - t0 < limit) {
                int a = Serial2.available();
                if (a > 0) n += Serial2.read(got + n, std::min((size_t)a, sizeof got - n));
            }
            int bad = 0;
            for (size_t i = 0; i < n; i++) bad += got[i] != pat[i];
            if (n == sizeof got && !bad) out.push_back(fmt("%u baud: 256 bytes came back intact", (unsigned)speeds[s]));
            else {
                ok = false;
                out.push_back(n ? fmt("%u baud: %u of 256 bytes came back, %d wrong", (unsigned)speeds[s], (unsigned)n, bad)
                                : fmt("%u baud: nothing came back", (unsigned)speeds[s]));
            }
            Serial2.end();
        }
        macBegin(cfg.baud);
        out.push_back(ok ? "LOOPBACK OK" : "LOOPBACK FAILED");
        if (!ok) {
            out.push_back("Check: a wire joins DB9 terminals 2 and 3; module VCC on 3V3 and");
            out.push_back("GND on GND; module TX and RX on GPIO16 and GPIO17.");
        }
        return ok;
    }

    bool tlsTest(const std::string &host, std::vector<std::string> &out)
    {
        if (::transport.busy()) { out.push_back("The AI relay is using the connection; try again after its reply."); return false; }
        uint32_t before = ESP.getFreeHeap(), t0 = millis();
        WiFiClientSecure &c = tlsClient;                // the relay's client: same CA bundle, same settings
        c.setHandshakeTimeout(20);
        if (!c.connect(host.c_str(), 443)) {
            char e[120] = "";
            c.lastError(e, sizeof e);
            out.push_back("TLS FAILED: " + host + ": " + (e[0] ? e : "no connection"));
            return false;
        }
        uint32_t hs = millis() - t0, during = ESP.getFreeHeap();
        c.print(("HEAD / HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: PlusModem\r\nConnection: close\r\n\r\n").c_str());
        std::string status;
        for (uint32_t t = millis(); millis() - t < 10000 && status.find('\n') == std::string::npos;) {
            int ch = c.read();
            if (ch >= 0) status += (char)ch;
            else if (!c.connected()) break;
            else delay(5);
        }
        c.stop();
        out.push_back("TLS OK: " + host + " answered " + trim(status.substr(0, status.find('\r'))));
        out.push_back(fmt("handshake %u ms; heap %u KB before, %u KB during (TLS %u KB), lowest %u KB", (unsigned)hs,
                          (unsigned)(before / 1024), (unsigned)(during / 1024), (unsigned)((before - during) / 1024),
                          (unsigned)(ESP.getMinFreeHeap() / 1024)));
        return true;
    }
    uint32_t freeHeap() { return ESP.getFreeHeap(); }
    uint32_t minFreeHeap() { return ESP.getMinFreeHeap(); }
};

static EspDevice device;

// ------------------------------------------------------------------------ setup and loop
void boardSetup()
{
    Serial.setRxBufferSize(1024);
    Serial.begin(115200);
    loadConfig(cfg);
    detectPins();
    macBegin(cfg.baud);
    WiFi.persistent(false);                         // our own settings hold the networks, not the Wi-Fi driver's
    WiFi.setHostname("plusmodem");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);                   // wifiPoll() reconnects, to the strongest known network
    WiFi.setSleep(false);
    tlsClient.setCACertBundle(ca_bundle);
    modem = new Modem(cfg, device);
    macLine = new Line(*modem, macPort, false, 16384);
    usbLine = new Line(*modem, usbPort, true, 2048);
    modem->attach(macLine, usbLine);
    Serial.print("\r\n" PM_NAME " " PM_VERSION " ready. Type AT+HELP.\r\n");
    Serial.print(fmt("Mac line: %u baud, ", (unsigned)cfg.baud).c_str());
    Serial.print(("pins " + pinNote + "\r\n").c_str());
    Serial.print(cfg.nets.empty() ? "Wi-Fi: no networks set (python3 modem/tools/setup.py wifi).\r\n"
                                  : fmt("Wi-Fi: %u known networks; joining the strongest.\r\n", (unsigned)cfg.nets.size()).c_str());
}

void boardLoop()
{
    modem->poll();
    wifiPoll();
    // Wired after power-on? While nothing is detected and the Mac line is quiet, look again every 3 s.
    static uint32_t lastPins;
    if (!pinsFound && macLine->bytesIn == 0 && macLine->idle() && millis() - lastPins > 3000) {
        lastPins = millis();
        Serial2.end();
        detectPins();
        macBegin(cfg.baud);
        if (pinsFound) modem->log("[pins] " + pinNote);
    }
    delay(1);
}
