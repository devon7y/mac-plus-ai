#!/usr/bin/env python3
"""A small terminal on the Plus Modem's USB console.

    python3 modem/tools/console.py                 interactive (Control-] quits)
    python3 modem/tools/console.py AT+STATUS AT+HELP   send commands, print the answers, exit
    options: --port /dev/cu.usbserial-0001 (or socket://127.0.0.1:2324), --reset (restart the ESP32 first)

The USB console runs the same commands as the Mac's line (AT+HELP lists them). Keys typed are sent at
once and the modem echoes them, so it behaves like a real modem terminal: for +++ wait a second,
type +++, wait a second. Lines from the modem's own log start with [mac], [wifi] or [pins].
"""
import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pmlink  # noqa: E402


def main():
    args = sys.argv[1:]
    port, reset = None, False
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--port":
            port = args.pop(0)
        elif a == "--reset":
            reset = True
        else:
            sys.exit(__doc__)
    link = pmlink.Link(port, reset)
    if args:                                            # one-shot commands
        bad = 0
        for cmd in args:
            res, info = link.command(cmd)
            for line in info:
                print(line)
            print(res)
            bad |= res != "OK"
        sys.exit(1 if bad else 0)

    import termios
    import tty
    print("Plus Modem console on %s. Control-] quits." % link.name)
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    stop = threading.Event()

    def reader():
        last_cr = False
        while not stop.is_set():
            data = link.s.read(4096)
            if not data:
                continue
            out = bytearray()
            for b in data:                              # CR alone (the relay's lines) shows as a new line
                if b == 10 and last_cr:
                    last_cr = False
                    continue
                last_cr = b == 13
                out += b"\r\n" if b in (10, 13) else bytes([b])
            os.write(sys.stdout.fileno(), bytes(out).decode("mac_roman").encode("utf-8"))
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        tty.setraw(fd)
        while True:
            ch = os.read(fd, 64)
            if not ch or b"\x1d" in ch:
                break
            link.write(ch.replace(b"\n", b"\r"))
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        stop.set()
        print()


if __name__ == "__main__":
    main()
