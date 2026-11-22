// Plus Modem: the interfaces between the portable core (Hayes lines, AI relay) and the hardware.
// plusmodem.ino implements them on the ESP32; modem/tests/ implements them on the host.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// A serial line: the Mac's UART or the USB console.
class Port {
public:
    virtual ~Port() {}
    virtual int read(uint8_t *buf, int max) = 0;          // non-blocking
    virtual int room() = 0;                               // bytes that can be written without blocking
    virtual void write(const uint8_t *p, int n) = 0;      // may block until queued
    virtual void drain() {}                               // wait until everything has been sent
};

// The far end in data mode: a TCP connection or the built-in AI relay.
class Remote {
public:
    virtual ~Remote() {}
    virtual bool alive() = 0;                             // connected, or data still waiting
    virtual int available() = 0;
    virtual int read(uint8_t *buf, int max) = 0;
    virtual int write(const uint8_t *p, int n) = 0;       // bytes taken
    virtual void poll() {}
    virtual void hangup() = 0;                            // close (a TCP remote deletes itself)
    virtual std::string describe() = 0;                   // for AT+STATUS
};

// One outgoing HTTP(S) connection, for the relay's model providers.
class Transport {
public:
    virtual ~Transport() {}
    virtual bool open(const std::string &host, uint16_t port, bool tls, std::string &err) = 0;  // blocking
    virtual bool send(const char *p, size_t n) = 0;       // blocking, all or nothing
    virtual int recv(uint8_t *buf, size_t max) = 0;       // > 0 bytes, 0 none yet, -1 closed
    virtual void close() = 0;
};

struct ScanEntry { std::string ssid; int rssi; int channel; bool open, eap; };

class Config;

// Everything else the core needs from the board.
class Device {
public:
    virtual ~Device() {}
    virtual uint32_t ms() = 0;
    virtual uint32_t random32() = 0;
    virtual void log(const std::string &line) = 0;        // a line for the USB console (events, monitor)
    virtual bool wifiUp() = 0;
    virtual std::string wifiStatus() = 0;                 // one line for AT+STATUS
    virtual void wifiChanged() = 0;                       // the list of networks changed: reconnect
    virtual bool scan(std::vector<ScanEntry> &out) = 0;   // blocking, a few seconds
    virtual Remote *dialTcp(const std::string &host, uint16_t port) = 0;   // blocking; null on failure
    virtual Transport *transport() = 0;                   // the relay's single HTTP(S) connection
    virtual void save(const Config &c) = 0;               // write the settings to flash
    virtual void setBaud(uint32_t baud) = 0;              // the Mac port's speed
    virtual std::string pinInfo() = 0;                    // which GPIO is RX/TX and how it was found
    virtual bool loopback(std::vector<std::string> &out) = 0;           // AT+LOOP
    virtual bool tlsTest(const std::string &host, std::vector<std::string> &out) = 0;  // AT+TLS
    virtual uint32_t freeHeap() = 0;
    virtual uint32_t minFreeHeap() = 0;                   // lowest since boot
};
