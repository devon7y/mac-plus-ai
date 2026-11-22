"""A small TOML reader for Pythons older than 3.11 (no tomllib): enough for relay.toml.

Supports comments, [table], [a.b], [[array.of.tables]], dotted-free bare or quoted keys, and values:
"basic" (with \\" \\\\ \\n \\t \\uXXXX escapes) or 'literal' strings, integers, floats, true/false,
one-line arrays and one-line inline tables. Not supported: multi-line strings, dates.
"""
import re

_BARE = re.compile(r"[A-Za-z0-9_-]+")


class Parser:
    def __init__(self, s):
        self.s = s
        self.i = 0

    def ws(self):
        while self.i < len(self.s) and self.s[self.i] in " \t":
            self.i += 1

    def key(self):
        self.ws()
        c = self.s[self.i:self.i + 1]
        if c in "\"'":
            return self.string()
        m = _BARE.match(self.s, self.i)
        if not m:
            raise ValueError("bad key at %r" % self.s[self.i:self.i + 20])
        self.i = m.end()
        return m.group(0)

    def string(self):
        q = self.s[self.i]
        self.i += 1
        out = []
        while True:
            if self.i >= len(self.s):
                raise ValueError("unterminated string")
            c = self.s[self.i]
            self.i += 1
            if c == q:
                return "".join(out)
            if c == "\\" and q == '"':
                e = self.s[self.i]
                self.i += 1
                if e == "u":
                    out.append(chr(int(self.s[self.i:self.i + 4], 16)))
                    self.i += 4
                elif e == "U":
                    out.append(chr(int(self.s[self.i:self.i + 8], 16)))
                    self.i += 8
                else:
                    out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "b": "\b", "f": "\f"}[e])
            else:
                out.append(c)

    def value(self):
        self.ws()
        c = self.s[self.i:self.i + 1]
        if c in "\"'":
            return self.string()
        if c == "[":
            self.i += 1
            arr = []
            while True:
                self.ws()
                if self.s[self.i] == "]":
                    self.i += 1
                    return arr
                arr.append(self.value())
                self.ws()
                if self.s[self.i] == ",":
                    self.i += 1
        if c == "{":
            self.i += 1
            tab = {}
            while True:
                self.ws()
                if self.s[self.i] == "}":
                    self.i += 1
                    return tab
                k = self.key()
                self.ws()
                if self.s[self.i] != "=":
                    raise ValueError("expected = in inline table")
                self.i += 1
                tab[k] = self.value()
                self.ws()
                if self.s[self.i] == ",":
                    self.i += 1
        m = re.compile(r"true|false|[+-]?[0-9_]+(\.[0-9_]+)?([eE][+-]?[0-9]+)?").match(self.s, self.i)
        if not m:
            raise ValueError("bad value at %r" % self.s[self.i:self.i + 20])
        self.i = m.end()
        t = m.group(0)
        if t in ("true", "false"):
            return t == "true"
        t = t.replace("_", "")
        return float(t) if ("." in t or "e" in t.lower()) else int(t)


def _strip_comment(line):
    q = None
    for k, c in enumerate(line):
        if q:
            if c == "\\" and q == '"':
                continue
            if c == q:
                q = None
        elif c in "\"'":
            q = c
        elif c == "#":
            return line[:k]
    return line


def loads(text):
    root = {}
    cur = root
    for n, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw).strip()
        if not line:
            continue
        try:
            if line.startswith("[["):
                names = [x.strip().strip('"') for x in line[2:line.rindex("]]")].split(".")]
                t = root
                for nm in names[:-1]:
                    t = t.setdefault(nm, {})
                cur = {}
                t.setdefault(names[-1], []).append(cur)
            elif line.startswith("["):
                names = [x.strip().strip('"') for x in line[1:line.rindex("]")].split(".")]
                cur = root
                for nm in names:
                    cur = cur.setdefault(nm, {})
                    if isinstance(cur, list):
                        cur = cur[-1]
            else:
                p = Parser(line)
                k = p.key()
                p.ws()
                if p.s[p.i] != "=":
                    raise ValueError("expected =")
                p.i += 1
                cur[k] = p.value()
        except (ValueError, IndexError, KeyError) as e:
            raise ValueError("relay config line %d: %s" % (n, e)) from None
    return root
