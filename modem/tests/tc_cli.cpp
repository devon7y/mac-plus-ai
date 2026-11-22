// Host driver for test_textconv.py: runs textconv.cpp on records read from stdin.
//
//   tc_cli stream|tomac|frommac|decode < records > records
//
// A record is a 4-byte little-endian length and that many bytes. In "stream" mode the records are
// UTF-8 chunks fed to one PlainStream, and a length of 0xFFFFFFFF ends a case: finish(), pack() over
// all of the case's events, then one output record with the relay's wire form ("T <Mac Roman>\r" or
// "N\r" per event; empty T events are written too). The other modes answer each record with
// toMac(utf8Decode(r)), macToUtf8(r) or utf8Encode(utf8Decode(r)).
#include "../firmware/plusmodem/textconv.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool readRecord(std::string &rec, bool &endCase) {
    unsigned char h[4];
    if (fread(h, 1, 4, stdin) != 4) return false;
    uint32_t n = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
    endCase = n == 0xFFFFFFFFu;
    rec.assign(endCase ? 0 : n, '\0');
    return endCase || n == 0 || fread(&rec[0], 1, n, stdin) == n;
}

static void writeRecord(const std::string &s) {
    uint32_t n = (uint32_t)s.size();
    unsigned char h[4] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)};
    fwrite(h, 1, 4, stdout);
    fwrite(s.data(), 1, s.size(), stdout);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    std::string rec;
    bool endCase;
    if (!strcmp(mode, "stream")) {
        tc::PlainStream ps;
        std::vector<tc::Ev> evs;
        while (readRecord(rec, endCase)) {
            if (!endCase) {
                ps.feed(rec.data(), rec.size(), evs);
                continue;
            }
            ps.finish(evs);
            tc::pack(evs);
            std::string wire;
            for (size_t i = 0; i < evs.size(); i++)
                wire += evs[i].kind == 'N' ? std::string("N\r") : "T " + tc::toMac(evs[i].text) + "\r";
            writeRecord(wire);
            evs.clear();
            ps.reset();
        }
    } else if (!strcmp(mode, "tomac") || !strcmp(mode, "frommac") || !strcmp(mode, "decode")) {
        while (readRecord(rec, endCase)) {
            if (mode[0] == 't') writeRecord(tc::utf8ToMac(rec));
            else if (mode[0] == 'f') writeRecord(tc::macToUtf8(rec));
            else writeRecord(tc::utf8Encode(tc::utf8Decode(rec)));
        }
    } else {
        fprintf(stderr, "usage: tc_cli stream|tomac|frommac|decode < records\n");
        return 2;
    }
    return 0;
}
