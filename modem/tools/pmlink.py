"""Plus Modem: the USB serial link shared by console.py, setup.py and relaytest.py.

The ESP32's USB console is 115200 baud. A port may also be a pyserial URL such as
socket://127.0.0.1:2324 (the host simulator, modem/tests/sim.cpp). Without --port the first
/dev/cu.usbserial* or /dev/cu.SLAB_USBtoUART* is used (or $PLUSMODEM_PORT).
pyserial is needed; if it's missing, a venv with it is made in modem/build/venv and the tool reruns there.
"""
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
VENV = os.path.join(HERE, "..", "build", "venv")
RESULTS = ("OK", "ERROR", "NO DIALTONE", "NO CARRIER", "NO ANSWER", "BUSY")


def serial_module():
    try:
        import serial
        return serial
    except ImportError:
        py = os.path.join(VENV, "bin", "python")
        if os.path.realpath(sys.executable) == os.path.realpath(py):
            sys.exit("pyserial is missing in %s" % VENV)
        if not os.path.exists(py):
            print("Setting up pyserial in modem/build/venv (once)...", file=sys.stderr)
            subprocess.check_call([sys.executable, "-m", "venv", VENV])
            subprocess.check_call([py, "-m", "pip", "install", "-q", "--disable-pip-version-check", "pyserial==3.5"])
        os.execv(py, [py] + sys.argv)


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.SLAB_USBtoUART*"))
    return ports[0] if ports else None


class Link:
    def __init__(self, port=None, reset=False):
        serial = serial_module()
        port = port or os.environ.get("PLUSMODEM_PORT") or find_port()
        if not port:
            sys.exit("The ESP32 isn't on USB (no /dev/cu.usbserial*). Check the cable carries data; ls /dev/cu.*")
        self.name = port
        if "://" in port:
            self.s = serial.serial_for_url(port, timeout=0.05)
        else:
            # DTR and RTS both asserted (pyserial's default) leave the board's auto-reset circuit alone.
            self.s = serial.Serial(port, 115200, timeout=0.05)
            if reset:                                   # pulse EN to restart the ESP32 (shows the boot messages)
                self.s.dtr = False
                self.s.rts = True
                time.sleep(0.1)
                self.s.rts = False
                time.sleep(0.05)
                self.s.dtr = True
        self.pending = b""

    def write(self, data):
        if isinstance(data, str):
            data = data.encode("mac_roman")
        self.s.write(data)

    def read(self, timeout=0.05):
        """Whatever arrives within `timeout` seconds (bytes)."""
        end = time.time() + timeout
        out = b""
        while True:
            chunk = self.s.read(4096)
            out += chunk
            if time.time() >= end and not chunk:
                return out

    def lines(self, timeout):
        """Yield complete lines (CR, LF or CRLF ends a line; empty lines skipped) until `timeout`."""
        end = time.time() + timeout
        while time.time() < end:
            self.pending += self.s.read(4096)
            while True:
                cut = [i for i in (self.pending.find(b"\r"), self.pending.find(b"\n")) if i >= 0]
                if not cut:
                    break
                i = min(cut)
                line, self.pending = self.pending[:i], self.pending[i + 1:]
                if line.strip():
                    yield line.decode("mac_roman")

    def command(self, cmd, timeout=15):
        """Send one AT command; return (result, info lines). Echo and [log] lines are dropped."""
        self.pending += self.read(0.05)
        self.pending = b""                              # drop anything older
        self.write(cmd + "\r")
        info = []
        for line in self.lines(timeout):
            s = line.strip()
            if s.upper().startswith("AT") or s.startswith("["):
                continue
            if s in RESULTS or s.startswith("CONNECT"):
                return s, info
            info.append(line.rstrip())
        return "TIMEOUT", info

    def close(self):
        self.s.close()


def quote(s):
    """A value for AT+WIFI="...": backslash before quotes and backslashes."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
