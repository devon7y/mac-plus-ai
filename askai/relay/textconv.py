"""Text conversion for the Ask AI relay.

* Mac Roman <-> Unicode with CR line ends (``from_mac``, ``to_mac``).
* ``PlainStream``: turns a model's streamed Markdown into plain text for a 512x342 1-bit screen, as it
  streams. Headings lose their #, lists get bullets, code blocks are indented 4 spaces (fences
  dropped), bold/italic/strike markers and inline-code backticks are removed, links become
  "text (url)", tables become "a | b" rows, rules become a line of dashes. Nothing is wrapped: the Mac
  wraps. Output is a list of events: ("T", text) for text to append and ("N",) for a line break.

Streaming works line by line: a line's Markdown prefix is decided as soon as its first word is
complete; after that the text is released word by word (the last, possibly incomplete word is held
back, and so is an unfinished [link](url)). Fence lines, table rows and possible rules are held until
their newline.
"""
import re
import unicodedata

# ---------------------------------------------------------------------------------------- Mac Roman

# Characters Mac Roman lacks, mapped to something readable. Characters it has (curly quotes, dashes,
# bullet, ellipsis, accented letters, the few maths symbols) pass through unchanged.
_REPLACE = {
    "×": "x", "→": "->", "←": "<-", "⇒": "=>", "↔": "<->", "⇔": "<=>",
    "↑": "^", "↓": "v", "€": "EUR", "−": "-", "‐": "-", "‑": "-",
    "‒": "-", "―": "—", "‚": "‘", "„": "“", "′": "'",
    "″": '"', "…": "…", " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", "　": " ", "✓": "v", "✔": "v", "✗": "x",
    "✘": "x", "•": "•", "●": "•", "▪": "•", "◦": "•",
    "‣": "•", "⁃": "-", "½": "1/2", "¼": "1/4", "¾": "3/4",
    "⅓": "1/3", "⅔": "2/3", "²": "^2", "³": "^3", "¹": "^1", "≤": "≤",
    "≥": "≥", "≈": "≈", "≠": "≠", "∞": "∞", "•​": "•",
    "™": "(TM)", "℠": "(SM)", "№": "No.", "‰": "‰", "‹": "‹",
    "›": "›", "─": "-", "│": "|", "┌": "+", "┐": "+", "└": "+",
    "┘": "+", "├": "+", "┤": "+", "┬": "+", "┴": "+", "┼": "+",
    "\u0141": "L", "\u0142": "l", "\u0110": "D", "\u0111": "d", "\u0131": "i", "\u0127": "h",
    "\u0126": "H", "\u00d0": "D", "\u00f0": "d", "\u00de": "Th", "\u00fe": "th", "\u2018": "\u2018",
    "\t": "    ",
}
_DROP = {"​", "‌", "‍", "⁠", "﻿", "️", "︎", "­"}


def _mac_char(ch):
    """One Unicode character as Mac Roman bytes (b'' to drop it)."""
    if ch in _DROP:
        return b""
    r = _REPLACE.get(ch)
    if r is not None:
        ch = r
        if len(ch) != 1:
            return "".join(ch).encode("mac_roman", "replace")
    o = ord(ch)
    if o < 32 or o == 127:
        return b""                                    # control characters (CR/LF are handled by the caller)
    try:
        b = ch.encode("mac_roman")
        if b == b"\xdb":                              # the euro sign: the Plus ROM fonts draw a currency sign
            return b"EUR"
        return b
    except UnicodeEncodeError:
        pass
    cat = unicodedata.category(ch)
    if cat in ("So", "Sk", "Cs", "Co", "Cn", "Mn", "Me", "Cf"):
        return b""                                    # emoji, symbols, combining marks: dropped
    d = unicodedata.normalize("NFKD", ch)
    out = b"".join(c.encode("mac_roman") for c in d if _encodable(c))
    return out if out else b"?"


def _encodable(c):
    try:
        c.encode("mac_roman")
        return unicodedata.category(c) != "Mn"
    except UnicodeEncodeError:
        return False


def to_mac(text):
    """Unicode (without line breaks) -> Mac Roman bytes."""
    try:
        b = text.encode("mac_roman")
        if b"\xdb" not in b and not any(ch in _REPLACE or ch in _DROP or ord(ch) < 32 for ch in text):
            return b
    except UnicodeEncodeError:
        pass
    return b"".join(_mac_char(ch) for ch in text)


def from_mac(data):
    """Mac Roman bytes from the Mac -> Unicode (line breaks become \\n)."""
    s = data.decode("mac_roman")
    return s.replace("\r\n", "\n").replace("\r", "\n")


# ---------------------------------------------------------------------------------------- Markdown

_HR = re.compile(r"^ {0,3}([-*_])( *\1){2,} *$")
_SETEXT = re.compile(r"^ {0,3}=+ *$")
_HEADING = re.compile(r"^ {0,3}(#{1,6})(?: +|$)")
_BULLET = re.compile(r"^( *)[-*+] +")
_ORDERED = re.compile(r"^( *)(\d{1,9})([.)]) +")
_FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
_TABLE_SEP = re.compile(r"^ *\|?( *:?-+:? *\|)+( *:?-+:? *)?\|? *$")
_LINK = re.compile(r"(!?)\[([^\]\n]*)\]\(([^)\s]*)(?:\s+\"[^\"]*\")?\)")
_AUTOLINK = re.compile(r"<((?:https?|mailto|ftp):[^>\s]+)>")
_ESCAPE = re.compile(r"\\([\\`*_{}\[\]()#+\-.!|~>])")
_EMPH_EDGE = "*_~"


def _strip_word(w):
    """Remove emphasis markers (*, _, ~~) at the edges of one word, as Markdown would render them."""
    if not w or all(c in _EMPH_EDGE for c in w):
        return w                                       # "*" or "**" alone (e.g. 2 * 3): keep
    i = 0
    while i < len(w) and i < 3 and w[i] in _EMPH_EDGE:
        i += 1
    lead = w[:i]
    body = w[i:]
    # punctuation after the closing marker ("**bold**," or "*it*.")
    m = re.search(r"([*_~]{1,3})([.,;:!?)\]'\"’”]*)$", body)
    if m and m.start() > 0 and body[m.start() - 1] not in " ":
        body = body[:m.start()] + m.group(2)
    if lead and lead.strip("~") == "" and len(lead) == 1:
        body = lead + body                             # a single ~ is not strike-through
    # an opening marker after punctuation: ("**bold**)
    m2 = re.match(r"^([(\[\"'‘“]+)([*_~]{1,3})(?=\S)", body)
    if m2:
        body = m2.group(1) + body[m2.end():]
    return body


class PlainStream:
    """Incremental Markdown -> plain text. feed(text) and finish() return lists of events."""

    MAX_T = 200          # bytes of Mac Roman per T line

    def __init__(self):
        self.buf = ""            # text of the current line not yet processed
        self.decided = False     # the current line's prefix has been emitted
        self.in_code = False     # inside a fenced code block
        self.fence = ""
        self.in_icode = False    # inside an inline `code` span (may span words)
        self.started = False     # any text emitted yet (leading blank lines are dropped)
        self.newlines = 0        # line breaks owed (emitted lazily: trailing ones are dropped)
        self.line_has_text = False
        self.out = []

    # ---- output helpers
    def _text(self, s):
        if not s:
            return
        if not self.started and not self.in_code:
            s = s.lstrip(" ")
            if not s:
                return
        if self.newlines:
            for _ in range(min(self.newlines, 2)):
                self.out.append(("N",))
            self.newlines = 0
        self.started = True
        self.line_has_text = True
        self.out.append(("T", s))

    def _newline(self):
        if self.started:
            self.newlines += 1
        self.line_has_text = False

    def _flush_events(self):
        out, self.out = self.out, []
        return pack(out, self.MAX_T)

    # ---- inline processing of a run of complete words
    def _inline(self, s):
        if self.in_code:
            return s
        out = []
        pos = 0
        # split into inline-code and normal parts, tracking backtick state across calls
        while pos < len(s):
            j = s.find("`", pos)
            if j < 0:
                part, pos2 = s[pos:], len(s)
            else:
                part, pos2 = s[pos:j], j
            out.append(part if self.in_icode else self._prose(part))
            if j < 0:
                break
            k = j
            while k < len(s) and s[k] == "`":
                k += 1
            self.in_icode = not self.in_icode          # backticks themselves are dropped
            pos = k
        return "".join(out)

    @staticmethod
    def _drop_symbols(s):
        """Drop emoji and other symbols Mac Roman can't show, with one blank next to them."""
        out = []
        dropped = False
        for ch in s:
            if ch in _DROP or (unicodedata.category(ch) in ("So", "Sk", "Cs", "Co") and not _encodable(ch)
                               and ch not in _REPLACE):
                dropped = True
                continue
            if dropped and ch == " " and (not out or out[-1] == " "):
                dropped = False
                continue
            dropped = False
            out.append(ch)
        return "".join(out)

    def _prose(self, s):
        s = self._drop_symbols(s)
        s = _AUTOLINK.sub(r"\1", s)

        def link(m):
            img, text, url = m.group(1), m.group(2).strip(), m.group(3)
            if img:
                return text
            if not url or url == text or url == "mailto:" + text:
                return text or url
            return f"{text} ({url})" if text else url
        s = _LINK.sub(link, s)
        s = _ESCAPE.sub(r"\1", s)
        parts = re.split(r"( +)", s)
        return "".join(p if p.startswith(" ") or not p else _strip_word(p) for p in parts)

    # ---- line handling
    def _hold_line(self, line):
        """True if this (incomplete) line must wait for its newline before anything is shown."""
        st = line.lstrip(" ")
        if self.in_code:
            return bool(_FENCE.match(line)) or (st and set(st) <= set("`~") )
        if not st:
            return True
        if st[0] in "`~|" or _FENCE.match(line):
            return True
        if st[0] in "-*_=" and set(st) <= set("-*_= "):
            return True                                 # a rule or setext underline, maybe
        first = st.split(" ", 1)
        if len(first) == 1:
            return True                                 # first word still incomplete
        return False

    def _prefix(self, line):
        """Decide the current line's prefix; returns the rest of the line (text to stream)."""
        if self.in_code:
            return "    " + line
        m = _HEADING.match(line)
        if m:
            return line[m.end():]
        m = _BULLET.match(line)
        if m and not _HR.match(line):
            return m.group(1) + "• " + line[m.end():]
        m = _ORDERED.match(line)
        if m:
            return m.group(1) + m.group(2) + m.group(3) + " " + line[m.end():]
        if line.startswith(">"):
            return line                                 # quotes stay as "> text"
        return line

    def _complete_line(self, line):
        """A whole line (without its newline) that was held back or never decided."""
        if self.in_code:
            if _FENCE.match(line) and line.strip()[0] == self.fence[0] and len(line.strip()) >= len(self.fence) \
                    and set(line.strip()) <= set(self.fence[0]):
                self.in_code = False
                return False                            # closing fence: no line at all
            self._text("    " + line if line.strip() else "")
            return True
        m = _FENCE.match(line)
        if m:
            self.in_code = True
            self.in_icode = False
            self.fence = m.group(1)
            return False
        st = line.strip()
        if not st:
            return True                                 # blank line: a paragraph break
        if _HR.match(line):
            self._text("—" * 12)
            return True
        if _SETEXT.match(line):
            return False
        if st.startswith("|") or (st.endswith("|") and "|" in st[:-1]):
            if _TABLE_SEP.match(line):
                return False
            cells = [c.strip() for c in st.strip("|").split("|")]
            self.in_icode = False
            self._text(" | ".join(self._inline(c) for c in cells))
            self.in_icode = False
            return True
        self._text(self._inline(self._prefix(line)))
        return True

    def _release(self, rest, final):
        """Stream the decided part of the current line: everything up to the last blank, unless a
        [link]( is still open."""
        if final:
            cut = len(rest)
        else:
            cut = rest.rfind(" ") + 1
            if self.in_code:
                cut = len(rest)                         # code: no inline processing, release all
            else:
                k = self._open_link(rest)
                if k >= 0 and k < cut:
                    cut = k
        if cut > 0:
            self._text(self._inline(rest[:cut]))
        return rest[cut:]

    @staticmethod
    def _open_link(s):
        """Index of the first [ whose link isn't complete yet, or -1."""
        i = s.find("[")
        while i >= 0:
            j = s.find("]", i + 1)
            if j < 0:
                return i if len(s) - i < 300 else -1
            if j + 1 >= len(s):
                return i                                # "]" at the end: a "(" may follow
            if s[j + 1] == "(":
                k = s.find(")", j + 2)
                if k < 0:
                    return i if len(s) - i < 400 else -1
            i = s.find("[", j + 1)
        return -1

    def feed(self, text):
        text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\t", "    ")
        self.buf += text
        while True:
            nl = self.buf.find("\n")
            if nl < 0:
                break
            line, self.buf = self.buf[:nl], self.buf[nl + 1:]
            if self.decided:
                self._release(line, True)
                self.decided = False
                self._newline()
            else:
                if self._complete_line(line):
                    self._newline()
        # the incomplete rest of the current line
        if not self.decided and self.buf and not self._hold_line(self.buf):
            self.buf = self._prefix(self.buf)
            self.decided = True
        if self.decided:
            self.buf = self._release(self.buf, False)
        return self._flush_events()

    def finish(self):
        if self.buf:
            if self.decided:
                self._release(self.buf, True)
            else:
                self._complete_line(self.buf)
            self.buf = ""
        self.decided = False
        self.newlines = 0                               # trailing blank lines are dropped
        return self._flush_events()


def pack(events, max_t=200):
    """Merge adjacent ("T", text) events, then split them into pieces of at most max_t Mac Roman bytes
    (at a blank where possible)."""
    merged = []
    for ev in events:
        if ev[0] == "T" and merged and merged[-1][0] == "T":
            merged[-1] = ("T", merged[-1][1] + ev[1])
        else:
            merged.append(ev)
    final = []
    for ev in merged:
        if ev[0] != "T":
            final.append(ev)
            continue
        s = ev[1]
        while s:
            cut = len(s)
            while len(to_mac(s[:cut])) > max_t:
                cut = max(1, cut * 3 // 4)
            if cut < len(s):                           # prefer to split after a blank
                sp = s.rfind(" ", 0, cut)
                if sp > cut // 2:
                    cut = sp + 1
            final.append(("T", s[:cut]))
            s = s[cut:]
    return final


def render(markdown, chunk=None):
    """Whole-text helper (tests): returns the plain text, with \\n for N events."""
    ps = PlainStream()
    evs = []
    if chunk:
        for i in range(0, len(markdown), chunk):
            evs += ps.feed(markdown[i:i + chunk])
    else:
        evs += ps.feed(markdown)
    evs += ps.finish()
    return "".join("\n" if e[0] == "N" else e[1] for e in evs)
