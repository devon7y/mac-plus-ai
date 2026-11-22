// Plus Modem: the Hayes command interpreter. One Line per serial port (the Mac's UART and the USB
// console run the same commands); the Modem holds the settings, dials and runs the AT+ commands.
#pragma once
#include <string>
#include <vector>
#include "config.h"
#include "iface.h"
#include "relay.h"

enum { RES_NONE = -1, RES_OK = 0, RES_CONNECT = 1, RES_NOCARRIER = 3, RES_ERROR = 4, RES_NODIALTONE = 6, RES_BUSY = 7, RES_NOANSWER = 8 };

class Modem;

class Line {
public:
    Line(Modem &m, Port &port, bool usb, size_t ringSize);
    void poll();
    void profile();                                 // ATZ: the saved E/V/Q (the USB console: E1 V1 Q0)
    void send(const std::string &s);                // raw bytes to the port (command mode)
    void info(const std::string &text);             // one line of a command's answer
    void result(int code);
    void connect(Remote *r);                        // a dial succeeded: online after CONNECT
    void drop();                                    // hang up the remote, if any
    bool online() const { return remote_ && !cmdMode_; }
    bool idle() const { return !remote_ && cmd_.empty(); }
    Remote *remote() const { return remote_; }
    Port &port() { return port_; }

    const bool usb;
    bool echo, verbose, quiet, xon;
    uint8_t S[13];                                  // S2 escape character, S12 guard time (1/50 s)
    uint32_t bytesIn;                               // bytes ever received (the boot banner stops at the first)

private:
    Modem &m_;
    Port &port_;
    Remote *remote_;
    bool cmdMode_, infoStarted_, xoffed_;
    std::string cmd_;
    std::vector<uint8_t> ring_;                     // bytes from the remote on their way to the port
    size_t head_, count_;
    int plus_;
    uint32_t lastData_, lastPlus_;

    uint32_t guardMs() const { return S[12] * 20u; }
    void commandByte(uint8_t c);
    void dataByte(uint8_t c, uint32_t now, std::string &out);
    void execute(const std::string &line);
    void pump();
};

class Modem {
public:
    Modem(Config &cfg, Device &dev);
    void attach(Line *mac, Line *usb) { mac_ = mac; usb_ = usb; }
    void poll();
    int dial(Line &l, const std::string &rest);    // ATD...: returns the result code
    int extended(Line &l, const std::string &cmd); // AT+...: returns the result code (RES_NONE: already sent)
    void log(const std::string &line);             // to the USB console, unless it is online
    uint32_t baudOf(const Line &l) const { return l.usb ? 115200 : cfg.baud; }
    Line *mac() { return mac_; }

    Config &cfg;
    Device &dev;
    AiRelay relay;

private:
    Line *mac_, *usb_;
    uint32_t nextBanner_;
    void status(Line &l);
    void help(Line &l);
    int wifi(Line &l, const std::string &args, bool eap);
    int alias(Line &l, const std::string &args);
    int key(Line &l, const std::string &args, bool url);
    int model(Line &l, const std::string &rest);
};
