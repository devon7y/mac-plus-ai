#!/usr/bin/env python3
"""End-to-end check of the modem's built-in AI relay: speaks askai/relay/PROTOCOL.md as the Mac would.

    python3 modem/tools/relaytest.py                       the free mock model over USB (no network, no key)
    python3 modem/tools/relaytest.py --model claude --ask "Say hi in five words."   a real model (costs a little)
    options: --port P (or socket://127.0.0.1:2323 for the simulator's Mac line), --pin PIN

Dials like Ask AI (AT, ATE0V1, ATDT ai), then HELLO, MODELS, USE, SYSTEM, ASK (the reply is printed as
the Mac would show it), PING, a STOP in the middle of a long mock reply, BYE, and checks NO CARRIER.
Exit status 0 if everything matched the protocol.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pmlink  # noqa: E402


class Fail(Exception):
    pass


def expect(link, pred, what, timeout=10):
    seen = []
    for line in link.lines(timeout):
        if pred(line):
            return line, seen
        seen.append(line)
    raise Fail("no %s within %d s (saw: %s)" % (what, timeout, seen[-5:]))


def ask(link, text, timeout=120):
    t0 = time.time()
    link.write("ASK %s\r" % text)
    line, _ = expect(link, lambda s: s.startswith("BEGIN ") or s.startswith("ERR "), "BEGIN", 30)
    if line.startswith("ERR "):
        raise Fail(line)
    shown, first, nbytes = "", None, 0
    for line in link.lines(timeout):
        if line.startswith("T "):
            first = first or time.time() - t0
            shown += line[2:]
            nbytes += len(line) + 1
        elif line == "N":
            shown += "\n"
            nbytes += 2
        elif line.startswith("END "):
            return line, shown, first, nbytes, time.time() - t0
        elif line.startswith("ERR "):
            raise Fail("reply failed: " + line)
        elif line.startswith("NOTE "):
            print("  " + line)
    raise Fail("no END within %d s" % timeout)


def main():
    args = sys.argv[1:]
    opt = {"--port": None, "--model": "mock", "--ask": "Hello from relaytest!", "--pin": "-"}
    while args:
        k = args.pop(0)
        if k not in opt or not args:
            sys.exit(__doc__)
        opt[k] = args.pop(0)
    link = pmlink.Link(opt["--port"])
    try:
        link.write("\r")
        res, _ = link.command("AT", 3)
        if res != "OK":                               # maybe still online from an earlier run
            time.sleep(1.2)
            link.write("+++")
            time.sleep(1.2)
            link.command("ATH")
        for cmd in ("AT", "ATE0V1"):
            res, _ = link.command(cmd)
            if res != "OK":
                raise Fail("%s -> %s" % (cmd, res))
        link.write("ATDT ai\r")
        line, _ = expect(link, lambda s: s.startswith("CONNECT") or s in pmlink.RESULTS, "CONNECT")
        if not line.startswith("CONNECT"):
            raise Fail("ATDT ai -> " + line)
        greet, _ = expect(link, lambda s: s.startswith("ASKAI "), "ASKAI greeting")
        print("connected:", greet)
        link.write("HELLO 1 %s\r" % opt["--pin"])
        hello, _ = expect(link, lambda s: s.startswith("OK HELLO") or s.startswith("ERR") or s.startswith("BYE"), "OK HELLO")
        if not hello.startswith("OK HELLO"):
            raise Fail(hello)
        print(hello)
        link.write("MODELS\r")
        done, models = expect(link, lambda s: s.startswith("OK MODELS"), "OK MODELS")
        print("models:", ", ".join(m.split()[1] for m in models if m.startswith("MODEL ")), "|", done)
        link.write("USE %s\r" % opt["--model"])
        use, _ = expect(link, lambda s: s.startswith("OK USE") or s.startswith("ERR"), "OK USE")
        if not use.startswith("OK USE"):
            raise Fail(use)
        link.write("SYSTEM Answer briefly.\r")
        expect(link, lambda s: s == "OK SYSTEM", "OK SYSTEM")
        end, shown, first, nbytes, total = ask(link, opt["--ask"])
        print("--- reply ---\n%s\n--- %s; first text after %.1f s, %d bytes in %.1f s" % (shown, end, first or 0, nbytes, total))
        if not end.startswith("END done") and not end.startswith("END length"):
            raise Fail("unexpected " + end)
        link.write("PING\r")
        expect(link, lambda s: s == "PONG", "PONG")
        if opt["--model"] == "mock":                 # STOP in the middle of a long reply
            link.write("ASK !long\r")
            expect(link, lambda s: s.startswith("BEGIN "), "BEGIN")
            time.sleep(2.0)
            link.write("STOP\r")
            stop, _ = expect(link, lambda s: s.startswith("END "), "END after STOP", 15)
            if not stop.startswith("END stopped"):
                raise Fail("STOP gave " + stop)
            print("STOP:", stop)
            time.sleep(1.2)                          # +++ keeps the line; ATO goes back
            link.write("+++")
            expect(link, lambda s: s == "OK", "OK after +++", 4)
            link.write("ATO\r")
            expect(link, lambda s: s.startswith("CONNECT"), "CONNECT after ATO")
            link.write("PING\r")
            expect(link, lambda s: s == "PONG", "PONG after ATO")
            print("+++ / ATO: ok")
        link.write("BYE\r")
        expect(link, lambda s: s == "OK BYE", "OK BYE")
        expect(link, lambda s: s == "NO CARRIER", "NO CARRIER")
        if opt["--model"] == "mock":                 # ATH hangs up a held connection
            link.write("ATDT ai\r")
            expect(link, lambda s: s.startswith("ASKAI "), "ASKAI greeting")
            time.sleep(1.2)
            link.write("+++")
            expect(link, lambda s: s == "OK", "OK after +++", 4)
            res, _ = link.command("ATH")
            if res != "OK":
                raise Fail("ATH -> " + res)
            print("+++ / ATH: ok")
        res, _ = link.command("ATE1")                # back to the console's usual echo
        print("RELAY TEST OK")
    except Fail as e:
        print("RELAY TEST FAILED:", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
