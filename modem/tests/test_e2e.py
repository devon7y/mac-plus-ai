#!/usr/bin/env python3
"""End-to-end test on the host (about 30 s, nothing leaves 127.0.0.1, no real keys):

  the firmware core (tests/sim.cpp) <- the tools, as they're used over USB
       Mac line: relaytest.py (PROTOCOL.md) with the mock model and with Anthropic, OpenAI and Gemini
                 against tests/mock_api.py (chunked SSE over plain HTTP; it checks every request);
                 then ATDT to askai's own Python relay (TCP data mode), +++, ATO, ATH
       USB line: console.py commands, setup-style AT+URL / AT+KEY with test keys

    python3 modem/tests/test_e2e.py           (run_host_tests.sh builds build/tests/sim first)
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
import pmlink  # noqa: E402

EXPECT = "Hello from the fake API: “quotes”, an emoji and\n\n• a list\n• of two"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(p, t=10):
    end = time.time() + t
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", p), 0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise SystemExit("port %d never opened" % p)


fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def main():
    mac, usb, api, relay = free_port(), free_port(), free_port(), free_port()
    procs = [subprocess.Popen([os.path.join(HERE, "..", "build", "tests", "sim"), str(mac), str(usb)], stderr=subprocess.DEVNULL),
             subprocess.Popen([sys.executable, "-I", "-B", os.path.join(HERE, "mock_api.py"), str(api)], stdout=subprocess.DEVNULL),
             subprocess.Popen([sys.executable, "-I", "-B", os.path.join(ROOT, "askai", "relay", "relay.py"), "--mock-only",
                               "--host", "127.0.0.1", "--port", str(relay)], stderr=subprocess.DEVNULL)]
    try:
        for p in (mac, usb, api, relay):
            wait_port(p)
        con = pmlink.Link("socket://127.0.0.1:%d" % usb)
        for cmd in ("AT+URL anthropic=http://127.0.0.1:%d" % api, "AT+URL openai=http://127.0.0.1:%d/v1" % api,
                    "AT+URL gemini=http://127.0.0.1:%d" % api, "AT+KEY anthropic=test-key-anthropic",
                    "AT+KEY openai=test-key-openai", "AT+KEY gemini=test-key-gemini"):
            res, info = con.command(cmd)
            check(res == "OK" and not any("test-key" in x for x in info), "USB: " + cmd.split("=")[0] + " (masked)")
        res, info = con.command("AT+MODEL")
        check(res == "OK" and any(x.startswith("* claude ") for x in info), "USB: AT+MODEL lists claude as the default")
        con.close()

        port = "socket://127.0.0.1:%d" % mac
        rt = os.path.join(TOOLS, "relaytest.py")
        r = subprocess.run([sys.executable, "-B", rt, "--port", port], capture_output=True, text=True, timeout=60)
        check(r.returncode == 0 and "RELAY TEST OK" in r.stdout and "END stopped" in r.stdout,
              "Mac: relaytest with the mock model (HELLO, MODELS, ASK, PING, STOP, BYE, NO CARRIER)")
        if r.returncode:
            print(r.stdout[-1500:])
        for model in ("claude", "claude-haiku", "gpt", "gemini"):
            r = subprocess.run([sys.executable, "-B", rt, "--port", port, "--model", model, "--ask", "hi"],
                               capture_output=True, text=True, timeout=60)
            got = r.stdout.split("--- reply ---\n", 1)[-1].split("\n--- END", 1)[0]
            check(r.returncode == 0 and got == EXPECT, "Mac: %s through the fake API (SSE, Markdown, Mac Roman)" % model)
            if r.returncode or got != EXPECT:
                print(r.stdout[-800:])

        # TCP data mode against askai's own relay
        m = pmlink.Link(port)
        m.command("ATE0V1")
        m.write("ATDT 127.0.0.1:%d\r" % relay)
        lines = list(m.lines(3))
        check(any(x.startswith("CONNECT 19200") for x in lines) and any(x.startswith("ASKAI 1 ") for x in lines),
              "Mac: ATDT to the Python relay: CONNECT 19200, then its ASKAI greeting")
        m.write("HELLO 1 -\r")
        check(any(x.startswith("OK HELLO") for x in m.lines(2)), "Mac: HELLO through the TCP connection")
        time.sleep(1.2)
        m.write("+++")
        check("OK" in [x.strip() for x in m.lines(2.5)], "Mac: +++ with guard times: OK, still connected")
        m.write("ATO\r")
        check(any(x.startswith("CONNECT") for x in m.lines(1)), "Mac: ATO: back online")
        m.write("PING\r")
        check("PONG" in list(m.lines(2)), "Mac: PING -> PONG after ATO")
        m.write("BYE\r")
        got = list(m.lines(3))
        check("OK BYE" in got and "NO CARRIER" in got, "Mac: BYE: the relay hangs up, NO CARRIER")
        m.write("ATDT 127.0.0.1:%d\r" % free_port())
        check("NO ANSWER" in list(m.lines(5)), "Mac: dialling a closed port: NO ANSWER")
        m.close()
    finally:
        for p in procs:
            p.terminate()
    print("%d failed" % len(fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
