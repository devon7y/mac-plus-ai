// textconv: a C++ port of askai/relay/textconv.py (see textconv.h). The Python regexes are written
// out as small matchers; each says which regex it replaces.
#include "textconv.h"
#include "macroman_tables.h"

namespace tc {

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static const size_t npos = ustr::npos;

// ------------------------------------------------------------------------------- character data

static bool inRanges(const uint32_t (*r)[2], size_t n, char32_t c) {
    size_t lo = 0, hi = n;                       // first range whose end >= c
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (r[mid][1] < c) lo = mid + 1; else hi = mid;
    }
    return lo < n && r[lo][0] <= c;
}

static bool isSpace(char32_t c) { return inRanges(tab::kSpace, COUNT(tab::kSpace), c); }   // str.isspace, \s
static bool isDigit(char32_t c) { return inRanges(tab::kDigit, COUNT(tab::kDigit), c); }   // re's \d

static int macByte(char32_t c) {                 // Python's mac_roman codec; -1 if not encodable
    if (c < 0x80) return (int)c;
    size_t lo = 0, hi = COUNT(tab::kUniToMac);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if ((tab::kUniToMac[mid] >> 8) < c) lo = mid + 1; else hi = mid;
    }
    if (lo < COUNT(tab::kUniToMac) && (tab::kUniToMac[lo] >> 8) == c) return tab::kUniToMac[lo] & 0xFF;
    return -1;
}

static const char *replaceOf(char32_t c) {       // _REPLACE, already as _mac_char() bytes
    size_t lo = 0, hi = COUNT(tab::kReplace);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tab::kReplace[mid].cp < c) lo = mid + 1; else hi = mid;
    }
    return lo < COUNT(tab::kReplace) && tab::kReplace[lo].cp == c ? tab::kReplace[lo].mac : 0;
}

static bool inDrop(char32_t c) {                 // _DROP
    for (size_t i = 0; i < COUNT(tab::kDrop); i++)
        if (tab::kDrop[i] == c) return true;
    return false;
}

static uint32_t runOf(char32_t c, char32_t &start) {   // payload of the kRuns entry covering c
    size_t lo = 0, hi = COUNT(tab::kRuns);       // last entry with start <= c
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if ((tab::kRuns[mid] >> 11) <= c) lo = mid; else hi = mid;
    }
    start = tab::kRuns[lo] >> 11;
    return tab::kRuns[lo] & 0x7FF;
}

// _drop_symbols' test: in _DROP, or So/Sk/Cs/Co and neither encodable nor in _REPLACE
static bool isDroppedSymbol(char32_t c) {
    if (inDrop(c)) return true;
    if (c < 0x80 || macByte(c) >= 0 || replaceOf(c)) return false;
    char32_t start;
    return runOf(c, start) == tab::kSym;
}

static void macChar(char32_t c, std::string &out) {   // _mac_char
    if (inDrop(c)) return;
    if (const char *r = replaceOf(c)) { out += r; return; }
    if (c < 32 || c == 127) return;              // control characters
    int b = macByte(c);
    if (b >= 0) {
        if (b == 0xDB) out += "EUR";             // the Plus ROM fonts draw a currency sign there
        else out += (char)b;
        return;
    }
    char32_t start;
    uint32_t p = runOf(c, start);
    if (p == tab::kQuest) out += '?';
    else if (p >= tab::kMultiBase) out += tab::kMulti[p - tab::kMultiBase];
    else if (p >= tab::kSeq) out += (char)(p - tab::kSeq + (c - start));
    // kSym, kDropped: nothing
}

std::string toMac(const ustr &s) {
    std::string out;
    out.reserve(s.size());
    bool plain = true;                           // Python's fast path: keeps DEL (0x7F), as it does
    for (size_t i = 0; i < s.size() && plain; i++) {
        char32_t c = s[i];
        int b = macByte(c);
        if (b < 0 || b == 0xDB || c < 32 || inDrop(c) || replaceOf(c)) plain = false;
        else out += (char)b;
    }
    if (plain) return out;
    out.clear();
    for (size_t i = 0; i < s.size(); i++) macChar(s[i], out);
    return out;
}

// ------------------------------------------------------------------------------------- UTF-8

// Decode p[0..n). Invalid input becomes U+FFFD per maximal ill-formed subpart, as Python's "replace"
// does. If rest is given, an incomplete sequence at the end is stored there instead.
static void decode(const char *p, size_t n, ustr &out, std::string *rest) {
    size_t i = 0;
    while (i < n) {
        unsigned char b = (unsigned char)p[i];
        if (b < 0x80) { out += (char32_t)b; i++; continue; }
        int need;
        char32_t c;
        unsigned char lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) { need = 1; c = b & 0x1F; }
        else if (b >= 0xE0 && b <= 0xEF) { need = 2; c = b & 0x0F; if (b == 0xE0) lo = 0xA0; if (b == 0xED) hi = 0x9F; }
        else if (b >= 0xF0 && b <= 0xF4) { need = 3; c = b & 0x07; if (b == 0xF0) lo = 0x90; if (b == 0xF4) hi = 0x8F; }
        else { out += (char32_t)0xFFFD; i++; continue; }
        size_t j = i + 1;
        int k = 0;
        for (; k < need && j < n; k++, j++) {
            unsigned char t = (unsigned char)p[j];
            if (t < lo || t > hi) break;
            c = (c << 6) | (t & 0x3F);
            lo = 0x80; hi = 0xBF;
        }
        if (k == need) { out += c; i = j; continue; }
        if (j >= n && rest) { rest->assign(p + i, n - i); return; }
        out += (char32_t)0xFFFD;
        i = j;
    }
}

ustr utf8Decode(const std::string &s) {
    ustr out;
    decode(s.data(), s.size(), out, 0);
    return out;
}

std::string utf8Encode(const ustr &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string utf8ToMac(const std::string &utf8) { return toMac(utf8Decode(utf8)); }

std::string macToUtf8(const std::string &mac) {
    ustr s;
    for (size_t i = 0; i < mac.size(); i++) {
        unsigned char b = (unsigned char)mac[i];
        if (b == '\r') {
            s += U'\n';
            if (i + 1 < mac.size() && mac[i + 1] == '\n') i++;
        } else {
            s += b < 0x80 ? (char32_t)b : (char32_t)tab::kMacToUni[b - 0x80];
        }
    }
    return utf8Encode(s);
}

// ------------------------------------------------------------------------------ string helpers

static bool has(const char *set, char32_t c) {
    for (; *set; set++)
        if ((unsigned char)*set == c) return true;
    return false;
}

static bool allIn(const ustr &s, const char *set) {
    for (size_t i = 0; i < s.size(); i++)
        if (!has(set, s[i])) return false;
    return true;
}

static ustr strip(const ustr &s) {               // str.strip()
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) a++;
    while (b > a && isSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static size_t spaces(const ustr &s, size_t i = 0) {   // index of the first non-blank at or after i
    while (i < s.size() && s[i] == ' ') i++;
    return i;
}

// ------------------------------------------------------------------------- line-start matchers

// _FENCE  ^ {0,3}(`{3,}|~{3,})
static bool matchFence(const ustr &line, ustr *fence = 0) {
    size_t n = spaces(line);
    if (n > 3 || n >= line.size() || (line[n] != '`' && line[n] != '~')) return false;
    size_t e = n;
    while (e < line.size() && line[e] == line[n]) e++;
    if (e - n < 3) return false;
    if (fence) *fence = line.substr(n, e - n);
    return true;
}

// _HR  ^ {0,3}([-*_])( *\1){2,} *$
static bool matchHr(const ustr &line) {
    size_t n = spaces(line);
    if (n > 3 || n >= line.size() || !has("-*_", line[n])) return false;
    int count = 0;
    for (size_t i = n; i < line.size(); i++) {
        if (line[i] == line[n]) count++;
        else if (line[i] != ' ') return false;
    }
    return count >= 3;
}

// _SETEXT  ^ {0,3}=+ *$
static bool matchSetext(const ustr &line) {
    size_t n = spaces(line);
    if (n > 3 || n >= line.size() || line[n] != '=') return false;
    size_t e = n;
    while (e < line.size() && line[e] == '=') e++;
    return spaces(line, e) == line.size();
}

// _HEADING  ^ {0,3}(#{1,6})(?: +|$)   -> end of the match
static bool matchHeading(const ustr &line, size_t &end) {
    size_t n = spaces(line);
    if (n > 3) return false;
    size_t e = n;
    while (e < line.size() && line[e] == '#') e++;
    if (e == n || e - n > 6 || (e < line.size() && line[e] != ' ')) return false;
    end = spaces(line, e);
    return true;
}

// _BULLET  ^( *)[-*+] +   -> indent (group 1) and end of the match
static bool matchBullet(const ustr &line, size_t &indent, size_t &end) {
    indent = spaces(line);
    if (indent + 1 >= line.size() || !has("-*+", line[indent]) || line[indent + 1] != ' ') return false;
    end = spaces(line, indent + 1);
    return true;
}

// _ORDERED  ^( *)(\d{1,9})([.)]) +   -> position of [.)] and end of the match
static bool matchOrdered(const ustr &line, size_t &mark, size_t &end) {
    size_t n = spaces(line), e = n;
    while (e < line.size() && isDigit(line[e])) e++;
    if (e == n || e - n > 9 || e + 1 >= line.size() || !has(".)", line[e]) || line[e + 1] != ' ') return false;
    mark = e;
    end = spaces(line, e + 1);
    return true;
}

// one cell of _TABLE_SEP:  *:?-+:? *
static bool sepCell(const ustr &s) {
    size_t a = spaces(s), b = s.size();
    while (b > a && s[b - 1] == ' ') b--;
    if (a < b && s[a] == ':') a++;
    if (a < b && s[b - 1] == ':') b--;
    if (a == b) return false;
    for (size_t i = a; i < b; i++)
        if (s[i] != '-') return false;
    return true;
}

// _TABLE_SEP  ^ *\|?( *:?-+:? *\|)+( *:?-+:? *)?\|? *$
// The '|'-separated parts (after a leading blank-only part and its '|') must be: one or more cells
// each followed by '|', then a last part that is blank or a cell; or the same with an empty part
// (the optional '|') before a blank last part.
static bool matchTableSep(const ustr &line) {
    std::vector<ustr> q;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('|', a);
        q.push_back(line.substr(a, b == npos ? npos : b - a));
        if (b == npos) break;
        a = b + 1;
    }
    if (spaces(q[0]) == q[0].size()) q.erase(q.begin());
    size_t n = q.size();
    for (size_t opt = 0; opt < 2; opt++) {
        if (n < 2 + opt) continue;
        size_t cells = n - 1 - opt;              // parts that must be "cell |"
        bool ok = true;
        for (size_t i = 0; i < cells && ok; i++) ok = sepCell(q[i]);
        const ustr &last = q[n - 1];
        bool blank = spaces(last) == last.size();
        if (ok && (opt == 0 ? blank || sepCell(last) : q[n - 2].empty() && blank)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- inline matchers

// _AUTOLINK  <((?:https?|mailto|ftp):[^>\s]+)>   -> index of the closing '>' for a '<' at i, or npos
static size_t autolinkEnd(const ustr &s, size_t i) {
    static const char32_t *const schemes[] = {U"https:", U"http:", U"mailto:", U"ftp:"};
    for (size_t k = 0; k < COUNT(schemes); k++) {
        ustr sch(schemes[k]);
        if (s.compare(i + 1, sch.size(), sch) != 0) continue;
        size_t q = i + 1 + sch.size(), e = q;
        while (e < s.size() && s[e] != '>' && !isSpace(s[e])) e++;
        return e > q && e < s.size() && s[e] == '>' ? e : npos;
    }
    return npos;
}

static ustr autolinks(const ustr &s) {           // _AUTOLINK.sub(r"\1")
    ustr out;
    for (size_t i = 0; i < s.size(); i++) {
        size_t e = s[i] == '<' ? autolinkEnd(s, i) : npos;
        if (e == npos) {
            out += s[i];
        } else {
            out.append(s, i + 1, e - i - 1);
            i = e;
        }
    }
    return out;
}

// _LINK  (!?)\[([^\]\n]*)\]\(([^)\s]*)(?:\s+\"[^\"]*\")?\)   with prose()'s link() as replacement
static ustr links(const ustr &s) {
    ustr out;
    size_t i = 0, n = s.size();
    while (i < n) {
        bool img = s[i] == '!' && i + 1 < n && s[i + 1] == '[';
        size_t b = img ? i + 1 : i;
        size_t end = npos, te = 0, us = 0, ue = 0;
        if (s[b] == '[') {
            te = b + 1;
            while (te < n && s[te] != ']' && s[te] != '\n') te++;
            if (te + 1 < n && s[te] == ']' && s[te + 1] == '(') {
                us = ue = te + 2;
                while (ue < n && s[ue] != ')' && !isSpace(s[ue])) ue++;
                if (ue < n && s[ue] == ')') {
                    end = ue + 1;
                } else if (ue < n) {             // blanks, then a "title"
                    size_t k = ue;
                    while (k < n && isSpace(s[k])) k++;
                    if (k < n && s[k] == '"') {
                        k = s.find('"', k + 1);
                        if (k != npos && k + 1 < n && s[k + 1] == ')') end = k + 2;
                    }
                }
            }
        }
        if (end == npos) { out += s[i++]; continue; }
        ustr text = strip(s.substr(b + 1, te - b - 1)), url = s.substr(us, ue - us);
        if (img) out += text;
        else if (url.empty() || url == text || url == U"mailto:" + text) out += text.empty() ? url : text;
        else if (text.empty()) out += url;
        else out += text + U" (" + url + U")";
        i = end;
    }
    return out;
}

static ustr escapes(const ustr &s) {             // _ESCAPE.sub(r"\1")  \\([\\`*_{}\[\]()#+\-.!|~>])
    ustr out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && has("\\`*_{}[]()#+-.!|~>", s[i + 1])) i++;
        out += s[i];
    }
    return out;
}

static bool isMark(char32_t c) { return c == '*' || c == '_' || c == '~'; }   // _EMPH_EDGE
static bool isClosePunct(char32_t c) { return has(".,;:!?)]'\"", c) || c == 0x2019 || c == 0x201D; }
static bool isOpenPunct(char32_t c) { return has("([\"'", c) || c == 0x2018 || c == 0x201C; }

static ustr stripWord(const ustr &w) {           // _strip_word
    size_t i = 0;
    while (i < w.size() && isMark(w[i])) i++;
    if (i == w.size()) return w;                 // "*" or "**" alone (e.g. 2 * 3): keep
    if (i > 3) i = 3;
    ustr lead = w.substr(0, i), body = w.substr(i);
    // ([*_~]{1,3})([.,;:!?)\]'"’”]*)$ : markers before trailing punctuation, not after a blank
    size_t p = body.size();
    while (p > 0 && isClosePunct(body[p - 1])) p--;
    if (p > 0 && isMark(body[p - 1])) {
        size_t q = p - 1;
        while (q > 0 && isMark(body[q - 1])) q--;
        size_t st = p >= 3 && p - 3 > q ? p - 3 : q;
        if (st > 0 && body[st - 1] != ' ') body = body.substr(0, st) + body.substr(p);
    }
    if (lead == U"~") body = lead + body;        // a single ~ is not strike-through
    // ^([(\["'‘“]+)([*_~]{1,3})(?=\S) : an opening marker after punctuation
    size_t o = 0;
    while (o < body.size() && isOpenPunct(body[o])) o++;
    if (o > 0) {
        size_t r = 0;
        while (o + r < body.size() && isMark(body[o + r])) r++;
        size_t k = 0;
        if (r > 3) k = 3;
        else if (r > 0 && o + r < body.size() && !isSpace(body[o + r])) k = r;
        else if (r >= 2) k = r - 1;
        if (k) body.erase(o, k);
    }
    return body;
}

static ustr dropSymbols(const ustr &s) {         // _drop_symbols
    ustr out;
    bool dropped = false;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (isDroppedSymbol(c)) { dropped = true; continue; }
        bool skip = dropped && c == ' ' && (out.empty() || out[out.size() - 1] == ' ');
        dropped = false;
        if (!skip) out += c;
    }
    return out;
}

// --------------------------------------------------------------------------------- PlainStream

PlainStream::PlainStream() { reset(); }

void PlainStream::reset() {
    pend_.clear();
    buf_.clear();
    decided_ = inCode_ = inIcode_ = started_ = false;
    fence_.clear();
    newlines_ = 0;
    out_.clear();
}

void PlainStream::text(ustr s) {
    if (s.empty()) return;
    if (!started_ && !inCode_) {
        s.erase(0, spaces(s));
        if (s.empty()) return;
    }
    for (int i = 0; i < newlines_ && i < 2; i++) out_.push_back(Ev{'N', ustr()});
    newlines_ = 0;
    started_ = true;
    out_.push_back(Ev{'T', s});
}

void PlainStream::newline() {
    if (started_) newlines_++;
}

void PlainStream::flush(std::vector<Ev> &out) {
    pack(out_);
    for (size_t i = 0; i < out_.size(); i++) out.push_back(out_[i]);
    out_.clear();
}

ustr PlainStream::inlineText(const ustr &s) {
    if (inCode_) return s;
    ustr out;
    size_t pos = 0;
    while (pos < s.size()) {                     // inline-code and normal parts; backticks are dropped
        size_t j = s.find('`', pos);
        ustr part = s.substr(pos, j == npos ? npos : j - pos);
        out += inIcode_ ? part : prose(part);
        if (j == npos) break;
        while (j < s.size() && s[j] == '`') j++;
        inIcode_ = !inIcode_;
        pos = j;
    }
    return out;
}

ustr PlainStream::prose(const ustr &in) const {
    ustr s = escapes(links(autolinks(dropSymbols(in))));
    ustr out;
    size_t i = 0;
    while (i < s.size()) {                       // re.split(r"( +)"): blanks kept, words stripped
        size_t j = i;
        if (s[i] == ' ') {
            j = spaces(s, i);
            out.append(s, i, j - i);
        } else {
            while (j < s.size() && s[j] != ' ') j++;
            out += stripWord(s.substr(i, j - i));
        }
        i = j;
    }
    return out;
}

bool PlainStream::holdLine(const ustr &line) const {
    ustr st = line.substr(spaces(line));
    if (inCode_) return matchFence(line) || (!st.empty() && allIn(st, "`~"));
    if (st.empty()) return true;
    if (has("`~|", st[0]) || matchFence(line)) return true;
    if (has("-*_=", st[0]) && allIn(st, "-*_= ")) return true;   // a rule or setext underline, maybe
    return st.find(' ') == npos;                 // first word still incomplete
}

ustr PlainStream::prefix(const ustr &line) const {
    if (inCode_) return U"    " + line;
    size_t a, b;
    if (matchHeading(line, b)) return line.substr(b);
    if (matchBullet(line, a, b) && !matchHr(line)) return line.substr(0, a) + U"• " + line.substr(b);
    if (matchOrdered(line, a, b)) return line.substr(0, a + 1) + U" " + line.substr(b);
    return line;                                 // quotes stay as "> text"
}

bool PlainStream::completeLine(const ustr &line) {
    if (inCode_) {
        ustr st = strip(line);
        if (matchFence(line) && st[0] == fence_[0] && st.size() >= fence_.size()
                && st.find_first_not_of(fence_[0]) == npos) {
            inCode_ = false;
            return false;                        // closing fence: no line at all
        }
        text(st.empty() ? ustr() : U"    " + line);
        return true;
    }
    ustr fence;
    if (matchFence(line, &fence)) {
        inCode_ = true;
        inIcode_ = false;
        fence_ = fence;
        return false;
    }
    ustr st = strip(line);
    if (st.empty()) return true;                 // blank line: a paragraph break
    if (matchHr(line)) {
        text(ustr(12, (char32_t)0x2014));
        return true;
    }
    if (matchSetext(line)) return false;
    if (st[0] == '|' || (st[st.size() - 1] == '|' && st.find('|') < st.size() - 1)) {
        if (matchTableSep(line)) return false;
        size_t a = 0, b = st.size();
        while (a < b && st[a] == '|') a++;
        while (b > a && st[b - 1] == '|') b--;
        ustr row;
        inIcode_ = false;
        for (size_t i = a;;) {
            size_t j = st.find('|', i);
            if (j == npos || j > b) j = b;
            if (i > a) row += U" | ";
            row += inlineText(strip(st.substr(i, j - i)));
            if (j == b) break;
            i = j + 1;
        }
        text(row);
        inIcode_ = false;
        return true;
    }
    text(inlineText(prefix(line)));
    return true;
}

// Stream the decided part of the current line: everything up to the last blank, unless a [link]( is
// still open.
static long openLink(const ustr &s) {            // index of the first [ whose link isn't complete yet
    size_t i = s.find('[');
    while (i != npos) {
        size_t j = s.find(']', i + 1);
        if (j == npos) return s.size() - i < 300 ? (long)i : -1;
        if (j + 1 >= s.size()) return (long)i;   // "]" at the end: a "(" may follow
        if (s[j + 1] == '(' && s.find(')', j + 2) == npos) return s.size() - i < 400 ? (long)i : -1;
        i = s.find('[', j + 1);
    }
    return -1;
}

ustr PlainStream::release(const ustr &rest, bool final) {
    size_t cut = rest.size();
    if (!final && !inCode_) {
        size_t sp = rest.rfind(' ');
        cut = sp == npos ? 0 : sp + 1;
        long k = openLink(rest);
        if (k >= 0 && (size_t)k < cut) cut = (size_t)k;
    }
    if (cut > 0) text(inlineText(rest.substr(0, cut)));
    return rest.substr(cut);
}

void PlainStream::feedText(const ustr &t) {
    for (size_t i = 0; i < t.size(); i++) {      // CRLF and CR -> LF, tab -> 4 blanks
        if (t[i] == '\r') {
            buf_ += U'\n';
            if (i + 1 < t.size() && t[i + 1] == '\n') i++;
        } else if (t[i] == '\t') {
            buf_ += U"    ";
        } else {
            buf_ += t[i];
        }
    }
    size_t nl;
    while ((nl = buf_.find('\n')) != npos) {
        ustr line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        if (decided_) {
            release(line, true);
            decided_ = false;
            newline();
        } else if (completeLine(line)) {
            newline();
        }
    }
    if (!decided_ && !buf_.empty() && !holdLine(buf_)) {   // the incomplete rest of the current line
        buf_ = prefix(buf_);
        decided_ = true;
    }
    if (decided_) buf_ = release(buf_, false);
}

void PlainStream::feed(const char *utf8, size_t n, std::vector<Ev> &out) {
    std::string bytes;
    bytes.swap(pend_);
    bytes.append(utf8, n);
    ustr t;
    decode(bytes.data(), bytes.size(), t, &pend_);
    feedText(t);
    flush(out);
}

void PlainStream::finish(std::vector<Ev> &out) {
    if (!pend_.empty()) {                        // a sequence cut off at the very end: U+FFFD
        ustr t;
        decode(pend_.data(), pend_.size(), t, 0);
        pend_.clear();
        feedText(t);
        flush(out);
    }
    if (!buf_.empty()) {
        if (decided_) release(buf_, true);
        else completeLine(buf_);
        buf_.clear();
    }
    decided_ = false;
    newlines_ = 0;                               // trailing blank lines are dropped
    flush(out);
}

// ---------------------------------------------------------------------------------------- pack

// Merge adjacent T events, then split them into pieces of at most maxT Mac Roman bytes (at a blank
// where possible). One character gives at most 4 bytes ("(TM)"); with maxT < 4 Python would loop
// forever on such a character, this stops at one character.
void pack(std::vector<Ev> &events, size_t maxT) {
    std::vector<Ev> merged;
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i].kind == 'T' && !merged.empty() && merged.back().kind == 'T') merged.back().text += events[i].text;
        else merged.push_back(events[i]);
    }
    std::vector<Ev> final;
    for (size_t i = 0; i < merged.size(); i++) {
        if (merged[i].kind != 'T') {
            final.push_back(merged[i]);
            continue;
        }
        const ustr &s = merged[i].text;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t len = s.size() - pos, cut = len;
            while (cut > 1 && toMac(s.substr(pos, cut)).size() > maxT) cut = cut * 3 / 4 > 1 ? cut * 3 / 4 : 1;
            if (cut < len) {                     // prefer to split after a blank
                size_t sp = s.rfind(' ', pos + cut - 1);
                if (sp != npos && sp >= pos && sp - pos > cut / 2) cut = sp - pos + 1;
            }
            final.push_back(Ev{'T', s.substr(pos, cut)});
            pos += cut;
        }
    }
    events.swap(final);
}

}  // namespace tc
