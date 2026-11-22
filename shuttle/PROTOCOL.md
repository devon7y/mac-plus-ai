# Shuttle protocol (version 1)

How the Shuttle app on the Mac Plus talks to `server/shuttle_server.py` on the MacBook. The link is
a serial line through a Hayes Wi-Fi modem (`ATDT mac` opens a TCP connection to the server), so it
can lose or garble bytes, and nothing paces it except the Mac. Everything here is designed for that.

## 1. Two kinds of units on the line

Both directions carry a mix of **text lines** and **frames**.

**Text lines** are Mac Roman, end with CR (0x0D), at most 255 bytes. LF (0x0A) and NUL (0x00) are
ignored everywhere outside a frame (so a modem that adds CR LF or telnet's CR NUL does no harm).
The modem's result codes (`OK`, `CONNECT 19200`, `NO CARRIER`, `PLUS MODEM READY`) and the
server's greeting are text lines.

**Frames** carry everything else, so every command, reply and data block is CRC-checked:

```
0x7E  <escaped body>  0x0D
body = type (1 byte)  id (1)  block (2, big-endian)  payload (0..1024)  CRC (2, big-endian)
```

- **CRC:** CRC-16/XMODEM (polynomial 0x1021, initial value 0, as in MacBinary II) over type, id,
  block and payload.
- **Escaping:** a body byte that is 0x0D, 0x11, 0x13, 0x7D, 0x7E or 0xFF is sent as 0x7D followed by
  the byte XOR 0x20. So a frame never contains a raw CR (which ends it), 0x7E (which starts one),
  XON/XOFF (so software flow control can stay on in the modem) or 0xFF (telnet IAC, in case a modem
  translates telnet). Overhead for random data: about 2.3%.
- **Receiving:** 0x7E starts a new frame anywhere (a half-received frame is dropped). CR ends the
  frame; a frame whose CRC is wrong or that is shorter than 6 bytes is "bad". Outside frames, bytes
  build text lines. A frame longer than 1030 body bytes is dropped.

Frame types:

| Type | Name | Meaning |
| --- | --- | --- |
| `C` (0x43) | command | Mac to server: a command line (section 3); `id` 0, `block` = command number |
| `R` (0x52) | reply | server to Mac: a reply line; `block` = the number of the command it answers |
| `D` (0x44) | data | block number `block` of transfer `id`; payload = that block's bytes |
| `A` (0x41) | ack | the receiver has every block before `block` (cumulative) |
| `N` (0x4E) | nak | resend starting at block `block` (also acknowledges every block before it) |

Block numbers are 16 bits and wrap; both sides resolve them relative to their own position, so the
window never spans more than a few blocks. Block *k* holds bytes *k·B* to *(k+1)·B − 1* of the
transfer (the last block may be shorter). A transfer of 0 bytes has no blocks.

## 2. Transfers: a sliding window, paced by the receiver

The sender keeps at most **W** blocks unacknowledged (go-back-N):

- it sends blocks while `next_to_send < acked + W`;
- **ack n**: everything before n arrived; the window moves on;
- **nak n**: everything before n arrived; send again from n;
- the sender's own timeout (no ack or nak for 15 s with blocks outstanding) sends again from the
  oldest unacknowledged block. After 6 such timeouts in a row it gives up.

The receiver accepts only the next block in order:

- the expected block, with the right length and a good CRC: kept; ack the new position;
- an older block (a duplicate): ack the current position again;
- a later block (one went missing) or a bad frame: nak the current position, but at most once per
  *nak wait* for the same position (blocks already in flight arrive after the nak and must not cause
  more naks). *nak wait* = W·B / (line bytes per second) + 1 s. Exception: if a block arrives whose
  number was already seen before that nak, the sender has gone back and the wanted block was lost
  again, so nak at once;
- nothing accepted for *nak wait* + 2 s: nak the current position again (that also repairs a lost
  ack). After 10 in a row the receiver gives up.

**The Mac can pause the sender.** When it receives a file it collects blocks in RAM (32 KB) and only
acks a block while its buffer still has room for W more. When the buffer is full it stops acking,
receives the blocks that are still in flight (at most W), and then the line is quiet. Only then does
it write the buffer to disk, then it acks and the sender continues. The Plus's floppy driver masks
interrupts while it reads or writes, so serial bytes arriving during disk I/O could be lost; this way
no data is ever on the line while the disk is busy. Sending works the same way: the Mac reads the
next 32 KB of the file only after every block read before has been acknowledged.

Defaults: **B = 1024 bytes, W = 4** (4 KB in flight, 2.1 s of line time at 19200 baud, so a Wi-Fi
round trip of up to about 2 s doesn't idle the line).

## 3. Session

After `CONNECT` the server greets with a text line:

```
SHUTTLE 1 <server name>
```

Then the Mac sends commands, one at a time, each in a `C` frame with a new command number (1, 2,
3, ...). Replies come in `R` frames carrying the same number: `OK <command> ...` or
`ERR <code> <message>`. If no reply arrives within 4 s the Mac sends the same frame again (same
number); the server answers a repeated number with its stored reply instead of running the command
twice. The server may also send `NOTE <text>` (an `R` frame) while it works, for example while it
unpacks a large archive; it resets the Mac's timeout and is shown in the status line.

For people testing by hand (`nc localhost 6801`), the server also accepts commands as plain text
lines and then answers with plain text lines.

| Command | Reply |
| --- | --- |
| `HELLO 1 <W> <B> <bytes/s>` | `OK HELLO 1 <W> <B> <server name>` (the server may lower W and B; bytes/s is the Mac's line speed, used for the nak wait) |
| `LIST <path>` | `OK LIST <id> <bytes> <entries>`, then the listing as a transfer, then `DONE <id>` |
| `GET <path>` | `OK GET <id> <bytes> <data fork> <resource fork>`, then the file as MacBinary II, then `DONE <id>` |
| `PUT <name> <bytes>` | `OK PUT <id>`; the Mac then sends the file as MacBinary II; the server replies `OK SAVED <id> <name>` (the name it was saved as) or `ERR ...` |
| `CANCEL` | stops the current transfer: `OK CANCEL` |
| `PING` | `OK PING` |
| `BYE` | `OK BYE`, then the server closes the connection (the modem says `NO CARRIER`) |

`DONE <id>` (an `R` frame with the command's number) is sent when the server receives the ack for
the last block. If it is lost, the Mac acks again every 2 s until `DONE` arrives (5 tries); the
server answers an ack of a finished transfer with `DONE` again. After the last block of an upload the
Mac waits for `OK SAVED`, sending the last block again every 5 s; the server answers a repeated
block with `OK SAVED` again. A new command while a transfer is
running ends that transfer first. The transfer `id` (1..255) is in every frame; frames of an older
transfer are ignored.

**Paths** are Mac style: names separated by `:`, relative to the served folder; empty or `:` is the
top. Names are Mac Roman, at most 31 bytes, never contain `:`.

**Listing** (the payload of a LIST transfer): one entry per CR-terminated line, folders first, then
files, each group sorted by name:

```
D <items> <name>                                   a folder
A <bytes> <name>                                   an archive (.sit .hqx .sea .cpt): open it like a folder
F <data> <resource> <type> <creator> <name>        a file; type and creator as 8 hex digits
```

Error codes: `VERSION`, `BADCMD`, `NOTFOUND`, `BADARCHIVE`, `TOOBIG`, `BADFILE`, `IO`.

## 4. Files

Everything the server sends with GET is **MacBinary II**: a 128-byte header (name, type, creator,
Finder flags, fork lengths, dates, CRC), the data fork padded to a multiple of 128, then the resource
fork padded the same way. The Mac writes both forks, the type, creator and Finder flags (clearing
"inited", "on desk", "changed" and "busy", so the Finder places the file and reads its icon).
Uploads (PUT) are MacBinary II made by the Mac the same way.

What the server offers (see `server/shuttle_server.py`):

| On the MacBook | Offered to the Plus as |
| --- | --- |
| a file with a resource fork or Finder type (native macOS metadata) | that file, both forks, its type and creator |
| `.bin` (valid MacBinary) | the file inside, unchanged |
| `.sit` `.hqx` `.sea` `.cpt` | an archive "folder": opening it unpacks it with `unar` (forks kept) and lists what's inside |
| a raw 400K / 800K / 1440K disk image (`.dsk` `.img` `.image`) | a Disk Copy 4.2 image (`dImg`/`dCpy`) |
| a Disk Copy 4.2 image | itself, typed `dImg`/`dCpy` |
| text (`.txt` `.md` `.c` ...) | `TEXT`/`ttxt`, converted to Mac Roman with CR line ends |
| anything else | the data fork, type `????` |

Uploads are saved in `From Plus/` as native macOS files (data fork, resource fork, type and creator
in the Finder info), so they can be sent back unchanged.

Frames are written `[type id block "payload"]` below.

## 5. Example

```
Mac: ATE0V1                 modem: OK
Mac: ATDTmac                modem: CONNECT 19200
                            server: SHUTTLE 1 My-MacBook
Mac: [C 0 1 "HELLO 1 4 1024 1912"]   server: [R 0 1 "OK HELLO 1 4 1024 My-MacBook"]
Mac: [C 0 2 "LIST :"]                 server: [R 0 2 "OK LIST 1 61 2"]
                                      server: [D 1 0 "D 1 0 ..."] (one block)
Mac: [A 1 1]                          server: [R 0 2 "DONE 1"]
Mac: [C 0 3 "GET Games:Lode Runner"]  server: [R 0 3 "OK GET 2 47360 0 46987"]
                                      server: [D 2 0] [D 2 1] [D 2 2] [D 2 3]
Mac: [A 2 1]                          server: [D 2 4]
...
Mac: [C 0 9 "BYE"]                    server: [R 0 9 "OK BYE"]     modem: NO CARRIER
```
