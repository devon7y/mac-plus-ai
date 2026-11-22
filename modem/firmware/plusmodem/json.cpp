#include "json.h"
#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *skipString(const char *p, const char *e)    // p at the opening quote
{
    for (p++; p < e; p++) {
        if (*p == '\\') p++;
        else if (*p == '"') return p + 1;
    }
    return 0;
}

static const char *skipValue(const char *p, const char *e, int depth)
{
    p = ws(p, e);
    if (p >= e || depth > 24) return 0;
    if (*p == '"') return skipString(p, e);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p++;
        for (;;) {
            p = ws(p, e);
            if (p >= e) return 0;
            if (*p == close) return p + 1;
            if (close == '}') {
                if (*p != '"' || !(p = skipString(p, e))) return 0;
                p = ws(p, e);
                if (p >= e || *p != ':') return 0;
                p++;
            }
            if (!(p = skipValue(p, e, depth + 1))) return 0;
            p = ws(p, e);
            if (p < e && *p == ',') p++;
        }
    }
    static const char *const words[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(words[i]);
        if (*p == words[i][0]) return (size_t)(e - p) >= n && !memcmp(p, words[i], n) ? p + n : 0;
    }
    const char *s = p;                                       // a number
    while (p < e && strchr("+-0123456789.eE", *p)) p++;
    return p > s ? p : 0;
}

static JVal make(const char *p, const char *end)
{
    JVal v = {0, p, (size_t)(end - p)};
    switch (*p) {
    case '"': v.type = 's'; v.p = p + 1; v.n = end - p - 2; break;
    case '{': v.type = 'o'; break;
    case '[': v.type = 'a'; break;
    case 't': v.type = 't'; break;
    case 'f': v.type = 'f'; break;
    case 'n': v.type = 'z'; break;
    default: v.type = 'n'; break;
    }
    return v;
}

JVal jget(const char *json, size_t n, const char *path)
{
    JVal none = {0, 0, 0};
    const char *p = ws(json, json + n), *e = json + n;
    while (*path) {
        const char *dot = strchr(path, '.');
        size_t klen = dot ? (size_t)(dot - path) : strlen(path);
        if (p >= e) return none;
        if (*p == '{') {
            p = ws(p + 1, e);
            bool found = false;
            while (p < e && *p == '"') {
                const char *k = p + 1, *kend = skipString(p, e);
                if (!kend) return none;
                bool match = (size_t)(kend - 1 - k) == klen && !memcmp(k, path, klen);
                p = ws(kend, e);
                if (p >= e || *p != ':') return none;
                p = ws(p + 1, e);
                if (match) { found = true; break; }
                if (!(p = skipValue(p, e, 0))) return none;
                p = ws(p, e);
                if (p < e && *p == ',') p = ws(p + 1, e);
            }
            if (!found) return none;
        } else if (*p == '[') {
            long idx = atol(path);
            if (klen == 0 || path[0] < '0' || path[0] > '9') return none;
            p = ws(p + 1, e);
            for (long i = 0; i < idx; i++) {
                if (p >= e || *p == ']' || !(p = skipValue(p, e, 0))) return none;
                p = ws(p, e);
                if (p < e && *p == ',') p = ws(p + 1, e);
            }
            if (p >= e || *p == ']') return none;
        } else {
            return none;
        }
        path += klen + (dot ? 1 : 0);
    }
    const char *end = skipValue(p, e, 0);
    if (!end || (p == ws(json, e) && ws(end, e) != e)) return none;   // the whole text must be one value
    return make(p, end);
}

static void putUtf8(std::string &o, unsigned long c)
{
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
}

static long hex4(const char *p, const char *e)
{
    if (e - p < 4) return -1;
    long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

std::string jstr(const JVal &v)
{
    std::string o;
    if (v.type != 's') return o;
    const char *p = v.p, *e = v.p + v.n;
    while (p < e) {
        char c = *p++;
        if (c != '\\' || p >= e) { o += c; continue; }
        c = *p++;
        switch (c) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': o += '\r'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
            long u = hex4(p, e);
            if (u < 0) { o += "\xEF\xBF\xBD"; break; }
            p += 4;
            if (u >= 0xD800 && u <= 0xDBFF) {                       // a surrogate pair
                long lo = (e - p >= 6 && p[0] == '\\' && p[1] == 'u') ? hex4(p + 2, e) : -1;
                if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                else u = 0xFFFD;
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            putUtf8(o, (unsigned long)u);
            break;
        }
        default: o += c; break;                                     // \" \\ \/
        }
    }
    return o;
}

long jnum(const JVal &v)
{
    if (v.type != 'n') return 0;
    return strtol(std::string(v.p, v.n).c_str(), 0, 10);
}

void jquote(std::string &out, const std::string &s)
{
    out += '"';
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static const char hx[] = "0123456789abcdef";
                out += "\\u00";
                out += hx[c >> 4];
                out += hx[c & 15];
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}
