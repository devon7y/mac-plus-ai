#!/usr/bin/env python3
"""Set up the Plus Modem from the MacBook over USB. Secrets are typed at hidden prompts and never shown.

    python3 modem/tools/setup.py wifi                  add a Wi-Fi network (name, then a hidden password)
    python3 modem/tools/setup.py eduroam               eduroam (username, password)
    python3 modem/tools/setup.py key anthropic         an API key at a hidden prompt
    python3 modem/tools/setup.py key openai --from-env OPENAI_API_KEY    the key from an environment variable
    python3 modem/tools/setup.py test [--loop]         AT+STATUS, and the loopback test with --loop
    option for all: --port /dev/cu.usbserial-0001

Providers for keys: anthropic, openai, gemini, openrouter, local (local needs an address instead:
console.py 'AT+URL local=http://192.168.1.20:11434/v1').
"""
import getpass
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pmlink  # noqa: E402

PROVIDERS = ("anthropic", "openai", "gemini", "openrouter", "local")


def send_secret(link, cmd):
    """Send a command that carries a secret; print only the modem's (masked) answer."""
    res, info = link.command(cmd)
    for line in info:
        print("  " + line)
    if res != "OK":
        sys.exit("The modem answered %s." % res)


def wait_wifi(link, seconds=40):
    print("Waiting for the modem to join (up to %d s)..." % seconds)
    end = time.time() + seconds
    while time.time() < end:
        res, info = link.command("AT+STATUS")
        wifi = [x for x in info if x.startswith("Wi-Fi:")]
        if wifi and "connected to" in wifi[0]:
            print("  " + wifi[0])
            return
        time.sleep(3)
    print("  " + (wifi[0] if wifi else "no answer"))
    print("Not connected yet. 2.4 GHz networks only; check the name and password (AT+SCAN lists what it sees).")


def main():
    args = sys.argv[1:]
    port = None
    if "--port" in args:
        i = args.index("--port")
        port = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    what = args[0]
    if what not in ("wifi", "eduroam", "key", "test"):
        sys.exit(__doc__)
    link = pmlink.Link(port)
    res, _ = link.command("AT")
    if res != "OK":
        sys.exit("The modem doesn't answer on %s (got %s). Is the firmware flashed?" % (link.name, res))

    if what == "wifi":
        ssid = input("Wi-Fi network name (2.4 GHz): ").strip()
        if not ssid:
            sys.exit("No name given.")
        pw = getpass.getpass("Password (hidden; empty for an open network): ")
        send_secret(link, "AT+WIFI=%s,%s" % (pmlink.quote(ssid), pmlink.quote(pw)))
        wait_wifi(link)
    elif what == "eduroam":
        user = input("eduroam username (e.g. abc123@example.edu): ").strip()
        if not user:
            sys.exit("No username given.")
        pw = getpass.getpass("eduroam password (hidden): ")
        q = pmlink.quote
        send_secret(link, "AT+WIFIEAP=%s,%s,%s,%s" % (q("eduroam"), q(user), q(user), q(pw)))
        wait_wifi(link, 60)
    elif what == "key":
        if len(args) < 2 or args[1] not in PROVIDERS:
            sys.exit("Which provider? One of: " + ", ".join(PROVIDERS))
        prov = args[1]
        if "--from-env" in args:
            var = args[args.index("--from-env") + 1]
            key = os.environ.get(var, "")
            if not key:
                sys.exit("%s is not set." % var)
        else:
            key = getpass.getpass("%s API key (hidden; empty clears it): " % prov)
        send_secret(link, "AT+KEY %s=%s" % (prov, key.strip()))
        res, info = link.command("AT+MODEL")
        print("Models (* = default; usable ones show in Ask AI's Model menu):")
        for line in info:
            print("  " + line)
    elif what == "test":
        res, info = link.command("AT+STATUS")
        for line in info:
            print(line)
        if "--loop" in args:
            print("Loopback test (DB9 terminals 2 and 3 must be joined):")
            res, info = link.command("AT+LOOP")
            for line in info:
                print("  " + line)
            sys.exit(0 if res == "OK" else 1)


if __name__ == "__main__":
    main()
