# Shuttle: files between the MacBook and the Mac Plus

Shuttle moves files between your MacBook and your Mac Plus (or a Mac SE) through the Wi-Fi modem on
the Plus's modem port. Files keep their resource forks, icons, types and creators. Slow but steady:
about 10 minutes per megabyte.

```
Mac Plus: Shuttle app ─ modem port ─ Wi-Fi modem ═ Wi-Fi ═ MacBook: shuttle_server.py, serving ~/Shuttle
```

There are three parts:

| Part | What it is |
| --- | --- |
| **Shuttle** | the program on the Plus: lists the files on the MacBook, downloads them, sends files back |
| **The server** | `shuttle/server/shuttle_server.py` on the MacBook: shares the folder `~/Shuttle` |
| **Two starter disks** | to get Shuttle (and Ask AI) onto the Plus in the first place, through a Mac SE FDHD |

## 1. Getting Shuttle onto the Plus (once)

The Plus can only read 400K/800K floppies, and USB floppy drives can't write those. A Mac SE FDHD (or any Mac with a SuperDrive) can.

1. Write `shuttle/build/starter_144.img` to a 1.44 MB (HD) floppy with a USB floppy drive (for example with
   `transfer/ufi_format.py`).
2. On the SE: follow **Read Me First** on that disk. Disk Copy 4.2 turns `Plus Modem.image` into an 800K
   floppy (any double-density 800K disk; it gets erased).
3. Boot the Plus from that 800K floppy. It starts at the Finder; double-click the **Plus Modem** disk to
   see **Shuttle**, **Ask AI** and a **Read Me** (System 6 with Narrow Screen, 420K free for downloads).

Both programs are already set up: Shuttle dials `mac`, Ask AI dials `ai` (the modem's built-in AI), both at
19200. If the Plus already has a System disk, you can instead copy just the two programs from the starter
disk onto any 800K floppy on the SE.

## 2. The server on the MacBook

```sh
cd ~/VS_Code/macintosh_plus
python3 shuttle/server/shuttle_server.py
```

It needs Python 3 (already on the MacBook) and, for StuffIt/BinHex archives, `unar` (`brew install unar`;
already installed). It prints:

```
Shuttle server 1.0, serving /Users/you/Shuttle
  Listening on port 6801. This MacBook's address on the network: 192.168.1.23
      AT+ALIAS mac=192.168.1.23:6801
```

- **Put files for the Plus in `~/Shuttle`** (made on the first run). Folders work.
- Files the Plus sends arrive in **`~/Shuttle/From Plus`**.
- **The first time**, macOS asks whether Python may accept incoming network connections: click **Allow**.
- Leave the Terminal window open while you use Shuttle; Control-C stops the server.
- Options: `--root <folder>` to serve another folder, `--port <n>` (default 6801).

**Tell the modem where the MacBook is** (once, and again whenever the MacBook's address changes): on the
Plus, open Shuttle, choose **Connection > Modem Terminal**, type the `AT+ALIAS mac=...` line exactly as the
server printed it and press Return (the modem answers OK and remembers it). Or type it in
`python3 modem/tools/console.py` with the modem on USB.

The Plus and the MacBook must be on the **same Wi-Fi network**, e.g. at home. Campus eduroam keeps devices
apart, so Shuttle can't reach the MacBook there (Ask AI still works: it only needs the internet).

### What the Plus sees

| In `~/Shuttle` | On the Plus |
| --- | --- |
| a Mac file with a resource fork or a Mac type (anything copied from a Mac disk, `unar` output) | that file, both forks, its icon, type and creator |
| `.bin` (MacBinary) | the file inside |
| `.sit`, `.hqx`, `.sea`, `.cpt` (also `.sit.bin` ...) | an **archive**: open it like a folder; the MacBook unpacks it, so you download the files inside |
| a raw 400K/800K/1.44 MB disk image (`.dsk`, `.img`, `.image`) | a **Disk Copy 4.2 image**, ready for Disk Copy |
| `.txt`, `.md` and other text | a TeachText document (Mac characters, Mac line ends) |
| anything else | a plain file |

## 3. Using Shuttle

- **Connect** (Command-K) dials `mac`. The status line says what's happening, then "Connected to
  <MacBook name>", and the list shows the Shuttle folder.
- **Double-click a folder** (or an archive) to open it; **Up** (or Command-Up) goes back.
  Arrow keys move the selection; typing the start of a name selects it; Return opens or downloads.
- **Download** (Command-D) asks where to save (the startup disk at first) and checks there's room first.
  The progress line shows how much is left and the time left. After saving, Shuttle reads the file back
  to check it ("Checking the saved file..."): a damaged floppy is caught and the bad copy deleted.
- **Send File...** (Command-U) sends a file from the Mac to `~/Shuttle/From Plus`.
- **Cancel** (Command-.) stops a transfer; a half-downloaded file is deleted.
- **Connection > Settings...**: what to dial (`mac`, or `host:port`), the modem init string (`ATE0V1`),
  speed (19200), handshake (None) and port (Modem). "Connect when Shuttle starts" saves a click.
- **Connection > Modem Terminal**: lines you type go straight to the modem (try `AT`, `AT+HELP`,
  `AT+STATUS`). Close the window when you're done.

Speed: 19200 bps moves about 1.9 KB per second on the line; with the pauses to write to the floppy and
the check afterwards, expect roughly **1.5 KB/s, about 11 minutes per megabyte** on a floppy (estimated;
MAME measured 94% of the line speed to a hard disk and 88% to a floppy, both including the
check, at its test speed).

## 4. When something goes wrong

| Message | What to do |
| --- | --- |
| "The modem doesn't answer" | Is the modem on and cabled to the **modem** port? Speed 19200 in Settings? Try `AT` in Modem Terminal. |
| "The modem couldn't reach "mac" (NO ANSWER / NO CARRIER)" | Is the server running? Same Wi-Fi? Type the `AT+ALIAS` line again (the MacBook's address may have changed). |
| "The modem isn't on Wi-Fi (NO DIALTONE)" | Set up Wi-Fi: `python3 modem/tools/setup.py wifi` on the MacBook. |
| "Connected, but no Shuttle server answered" | Something else answered at that address: check the `AT+ALIAS` line and the port (6801). |
| "There isn't room for it" | Save to another disk, or delete something. A floppy holds about 780K. |
| "The saved file didn't read back correctly" | The disk is probably bad: try another one. |
| "The connection to the MacBook was lost" | Wi-Fi dropped or the server stopped. Connect again. |

## 5. For developers

```sh
python3 -I shuttle/build.py                    # app, Ask AI, plus_modem_800k.img, Plus Modem.image, starter_144.img
python3 -I shuttle/server/test_server.py       # protocol tests, clean and lossy line, archives (about 25 s)
shuttle/tests/run_chain.sh                     # MAME: Plus 1 MB, list, 300 KB download, upload, cancel, then verify.py
TARGET=se7 shuttle/tests/run_chain.sh          # the same on the SE / System 7.0.1 testbed (SCSI disk)
```

| Path | What |
| --- | --- |
| `app/` | the Mac app (Retro68 C): `main.c` window and menus, `proto.c` session and transfers, `link.c` modem and framing, `files.c` MacBinary and forks, `serial.c` (from Ask AI) |
| `server/shuttle_server.py`, `server/test_server.py` | the server (standard library + `unar`) and its tests |
| `PROTOCOL.md` | the protocol: CRC frames, sliding window paced by the Mac, commands |
| `disk/` | the two Read Me texts |
| `tests/` | MAME scripts: `run_chain.sh`, `transfer.lua`, `verify.py`, `boot_finder.lua` (the Plus Modem disk boots and both apps open), `diskcopy_se.lua` (Disk Copy on the SE makes the 800K floppy), `sfdialog.lua` (redraw after a dialog), `hayes_emu.py` (Ask AI's modem emulator plus aliases and the boot banner) |
| `build/` (gitignored) | everything built, and `vendor/` with Disk Copy 4.2 |

**Disk Copy 4.2** (Apple, 1988–1991, freeware): `Disk_Copy_(v4.2).sit` from Macintosh Repository,
<https://www.macintoshrepository.org/2416-diskcopy-4-2-5-0-5-5-6-0-6-2-6-3-3-6-4-6-5b13-7-0-8-0>
(the 15.3 KB "Disk Copy v4.2 (1994)" entry; SHA-1 `8fd1b1faa3233dae7fdb20307758a2f0349caaef` as listed
there; SHA-256 `2f62f359df75301dca0074af072e6e2b09f487b4ee91c9de7ab10e986aaa7424`). Inside: "Disk Copy 4.2",
APPL/dCpy, resource fork 24,359 bytes, `vers` "4.2, Copyright © 1988-1991 by Apple Computer". `build.py`
expects the archive in `build/vendor/` and checks the SHA-256.

**MAME notes.** MAME clocks the Plus's serial chip 6.7% fast, so the tests use the app's hidden test speed
(prefs only: exactly 7,200 baud in MAME). MAME's null modem stalls for good if a byte arrives before the
Lua script has set its speed, so the emulated modem waits 0.5 s before its boot banner. When MAME saves a
written 800K floppy image it sometimes drops a few sectors (zeros); Shuttle's own read-back check passes
inside the emulator, and the hard-disk run is byte-exact, so `verify.py` reports those sectors as a note.
