// Plus Modem simulator: the firmware's portable core on the host, with TCP sockets for the two
// serial lines, so the tools and the end-to-end test can run without the ESP32.
//     sim [mac-port] [usb-port]       (default 2323 2324; connect with socket://127.0.0.1:2324)
// Dialling and HTTP use real sockets; there is no TLS, so providers must be http:// (AT+URL).
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../firmware/plusmodem/modem.h"

static uint32_t nowMs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static int listenOn(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr *)&a, sizeof a) || listen(s, 1)) { perror("listen"); exit(1); }
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}

static int connectTo(const std::string &host, uint16_t port, std::string &err)
{
    addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), fmt("%u", port).c_str(), &hints, &res) || !res) { err = "host not found"; return -1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r) { err = strerror(errno); close(s); return -1; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return s;
}

// A serial line: whoever is connected to the listening port.
struct SockPort : Port {
    int ls, fd = -1;
    explicit SockPort(int port) : ls(listenOn(port)) {}
    void accept_()
    {
        int c = accept(ls, 0, 0);
        if (c < 0) return;
        if (fd >= 0) close(fd);
        fd = c;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
    int read(uint8_t *b, int max)
    {
        accept_();
        if (fd < 0) return 0;
        ssize_t n = recv(fd, b, max, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd); fd = -1; return 0; }
        return n < 0 ? 0 : (int)n;
    }
    int room() { return 4096; }
    void write(const uint8_t *p, int n)
    {
        if (fd >= 0 && send(fd, p, n, 0) < 0) { close(fd); fd = -1; }
    }
};

struct SockRemote : Remote {
    int fd;
    std::string desc, buf;
    bool open = true;
    SockRemote(int f, const std::string &d) : fd(f), desc(d) {}
    void fill()
    {
        uint8_t b[1024];
        while (open) {
            ssize_t n = recv(fd, b, sizeof b, MSG_DONTWAIT);
            if (n > 0) buf.append((char *)b, n);
            else { if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) open = false; break; }
        }
    }
    bool alive() { fill(); return open || !buf.empty(); }
    int available() { fill(); return (int)buf.size(); }
    int read(uint8_t *p, int max) { int n = std::min(max, (int)buf.size()); memcpy(p, buf.data(), n); buf.erase(0, n); return n; }
    int write(const uint8_t *p, int n) { return open ? (int)send(fd, p, n, 0) : n; }
    void hangup() { close(fd); delete this; }
    std::string describe() { return "TCP " + desc; }
};

struct SockTransport : Transport {
    int fd = -1;
    bool open(const std::string &host, uint16_t port, bool tls, std::string &err)
    {
        close();
        if (tls) { err = "the simulator has no TLS (use AT+URL with http://)"; return false; }
        fd = connectTo(host, port, err);
        return fd >= 0;
    }
    bool send(const char *p, size_t n) { return fd >= 0 && ::send(fd, p, n, 0) == (ssize_t)n; }
    int recv(uint8_t *b, size_t max)
    {
        if (fd < 0) return -1;
        ssize_t n = ::recv(fd, b, max, MSG_DONTWAIT);
        if (n > 0) return (int)n;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
};

struct SimDevice : Device {
    SockTransport tr;
    uint32_t t0 = nowMs();
    uint32_t ms() { return nowMs() - t0; }
    uint32_t random32() { return (uint32_t)arc4random(); }
    void log(const std::string &l) { fprintf(stderr, "%s\n", l.c_str()); }
    bool wifiUp() { return true; }
    std::string wifiStatus() { return "simulated (the host's network)"; }
    void wifiChanged() { fprintf(stderr, "[wifi] (simulated) network list changed\n"); }
    bool scan(std::vector<ScanEntry> &v) { ScanEntry e = {"Simulated", -40, 6, false, false}; v.push_back(e); return true; }
    Remote *dialTcp(const std::string &h, uint16_t p)
    {
        std::string err;
        int fd = connectTo(h, p, err);
        return fd < 0 ? 0 : new SockRemote(fd, fmt("%s:%u", h.c_str(), p));
    }
    Transport *transport() { return &tr; }
    void save(const Config &) {}
    void setBaud(uint32_t b) { fprintf(stderr, "[sim] Mac line now %u baud\n", (unsigned)b); }
    std::string pinInfo() { return "simulator (no pins)"; }
    bool loopback(std::vector<std::string> &o) { o.push_back("LOOPBACK OK (simulated)"); return true; }
    bool tlsTest(const std::string &h, std::vector<std::string> &o) { o.push_back("the simulator has no TLS"); return false; }
    uint32_t freeHeap() { return 0; }
    uint32_t minFreeHeap() { return 0; }
};

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int macPort = argc > 1 ? atoi(argv[1]) : 2323, usbPort = argc > 2 ? atoi(argv[2]) : 2324;
    Config cfg;
    SimDevice dev;
    SockPort mac(macPort), usb(usbPort);
    Modem modem(cfg, dev);
    Line macLine(modem, mac, false, 16384), usbLine(modem, usb, true, 2048);
    modem.attach(&macLine, &usbLine);
    fprintf(stderr, "Plus Modem simulator: Mac line on 127.0.0.1:%d, USB console on 127.0.0.1:%d\n", macPort, usbPort);
    for (;;) {
        modem.poll();
        usleep(1000);
    }
}
