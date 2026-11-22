# Plus Modem

Firmware for a DIY Wi-Fi modem: an ESP32 (WROOM-32 DevKit, CP2102 USB) and a MAX3232 module on
the Mac Plus's modem port. Wiring: `HARDWARE.md`.

```
Mac Plus modem port -> mini-DIN-8 cable -> DB9 breakout -> MAX3232 -> ESP32 (UART2) -> Wi-Fi
                                                                     ESP32 (USB)  -> MacBook (set-up console)
```

It does two jobs:

1. **A Hayes-compatible Wi-Fi modem.** `ATDT host:port` opens a TCP connection. Ask AI (`askai/`) and
   Shuttle (`shuttle/`) dial through it.
2. **A built-in AI relay.** `ATDT ai` connects to a relay inside the ESP32. It speaks Ask AI's protocol
   (`askai/relay/PROTOCOL.md`) and calls the model APIs over HTTPS itself. No other computer is
   needed, and it works on eduroam, which blocks devices on the network from reaching each other.

## Quick start

All of these run on the MacBook with the ESP32 plugged in over USB.

| Step | Command |
| --- | --- |
| Build and flash (the first run installs the toolchain, about 0.8 GB in `modem/build/`) | `modem/tools/flash.sh` |
| Join a Wi-Fi network (2.4 GHz). The password is typed at a hidden prompt. | `python3 modem/tools/setup.py wifi` |
| Join eduroam (username and password) | `python3 modem/tools/setup.py eduroam` |
| Store an API key (hidden prompt, or `--from-env VAR`) | `python3 modem/tools/setup.py key anthropic` |
| Status, plus the loopback test (DB9 terminals 2 and 3 joined) | `python3 modem/tools/setup.py test --loop` |
| A terminal on the modem (Control-] quits) | `python3 modem/tools/console.py` |
| One-off commands | `python3 modem/tools/console.py AT+STATUS AT+SCAN` |
| Check the AI relay end to end (free mock model, no network) | `python3 modem/tools/relaytest.py` |

On the Plus:

- **Ask AI:** Connection > Settings: relay address `ai`, init string `ATE0V1`, speed 19200, handshake
  None, port Modem. Until a key is set, only the free "Mock Model" is in the Model menu.
- **Shuttle:** dial `mac`, after `AT+ALIAS mac=<MacBook IP>:6801`. Shuttle's server prints this line
  when it starts.

## Commands

Type `AT` first. The USB console and the Mac's line run the same commands. `AT+HELP` lists them.

| Command | What it does |
| --- | --- |
| `AT` | `OK` |
| `ATDT host:port`, `ATDThost:port`, `ATDT "host:port"`, `ATDT host port` | Dial: `CONNECT 19200`, then data mode. Errors: `NO DIALTONE` (not on Wi-Fi), `NO ANSWER` (unreachable), `ERROR` (bad address). Without a port, 23. |
| `ATDT alias` | Dial a stored alias (see `AT+ALIAS`) |
| `ATDT ai` | The built-in AI relay (`BUSY` if the other line is already using it) |
| `+++` | Back to command mode, still connected. Needs 1 s of silence before and after (S12). |
| `ATO` | Back online (`CONNECT`) |
| `ATH` | Hang up (`OK`) |
| `ATZ` | Hang up, reload the saved E/V/Q settings |
| `ATE0/1`, `ATV0/1`, `ATQ0/1` | Echo; word or number results (`0` OK, `1` CONNECT, `3` NO CARRIER, `4` ERROR, `6` NO DIALTONE, `7` BUSY, `8` NO ANSWER); quiet |
| `AT&F`, `AT&W`, `AT&V` | Factory E/V/Q; save them for the Mac line; show the settings |
| `ATI` | Name and version |
| `ATSn=v`, `ATSn?` | S registers 0 to 12: S2 is the escape character (43, `+`), S12 is the guard time in 1/50 s (50) |
| `AT&K0` / `AT&K4`, `X`, `M`, `&C`, `&D` and others | Accepted, so other programs' init strings work (`&K4` turns on XON/XOFF) |
| `AT+HELP` | The command list |
| `AT+STATUS` | Wi-Fi, IP, line speed, detected pins, both lines, keys (masked), default model, uptime, free memory |
| `AT+SCAN` | Wi-Fi networks in range, strongest first |
| `AT+WIFI="ssid","password"` | Add or replace a network (an open network: `AT+WIFI="ssid"`). Up to 8 are remembered. |
| `AT+WIFIEAP="ssid","identity","username","password"` | WPA2-Enterprise (PEAP), e.g. eduroam |
| `AT+WIFILIST`, `AT+WIFIDEL=n` | List the networks (passwords never shown), delete one |
| `AT+BAUD=n` | The Mac line's speed, 1200 to 57600 (default 19200). It answers `OK` at the old speed, then switches. Saved. |
| `AT+FLOW=NONE` or `XON` | XON/XOFF from the Mac pauses the modem's output (default NONE). Saved. Not for binary transfers. |
| `AT+ALIAS name=host:port` | Add or replace an alias; `name=` deletes it; `AT+ALIAS` lists them (up to 16) |
| `AT+MODEL` | The relay's models: `*` marks the default, and models without a key or address are marked |
| `AT+TLS=host` | HTTPS test against a host (default api.anthropic.com): handshake time and memory. No key is sent. |
| `AT+MON=0/1` | Show the Mac line's commands and results on the USB console (default 1) |

These work **from the USB console only**, so nobody at the Plus can change where keys go:

| Command | What it does |
| --- | --- |
| `AT+KEY provider=key` | Store an API key (`provider=` clears it). `AT+KEY` alone lists keys, masked (`sk-...abcd`), and addresses. |
| `AT+URL provider=url` | A provider's base address (`provider=` restores the default). Needed for `local`. |
| `AT+MODEL=id` | Choose the default model |
| `AT+MODEL id=provider/model,label,tag` | Add or replace a model (`id=` deletes it; `AT+MODEL RESET` restores the built-in list) |
| `AT+PIN=pin` | Require a PIN in Ask AI's HELLO (`AT+PIN=` removes it) |
| `AT+LOOP` | Loopback self-test through the MAX3232, with DB9 terminals 2 and 3 joined. Prints `LOOPBACK OK`. |

Providers: `anthropic`, `openai`, `gemini`, `openrouter`, `local` (Ollama or LM Studio, e.g.
`AT+URL local=http://192.168.1.20:11434/v1`, no key needed), and `mock`.

## Behaviour

- **Pins.** At power-on, GPIO16 and GPIO17 are read with pull-downs. The one held high is the MAX3232's
  output, so it becomes RX. The two data jumpers can therefore go either way round. If both read the
  same, it uses RX=16, TX=17 and says so. It then looks again every 3 s until something is detected,
  and again on `AT+LOOP`. The result is printed at boot and in `AT+STATUS`.
- **Boot banner.** For the first 60 s after power-on, `PLUS MODEM READY` goes to the Mac every 2 s,
  until the Mac sends its first byte (HARDWARE.md step 7).
- **The USB console.** 115200 baud, echo on. While the USB line isn't in data mode, it also shows the
  Mac line's commands and results (`[mac] ...`) and Wi-Fi events (`[wifi] ...`), with secrets masked.
- **Wi-Fi.** It scans, joins the strongest remembered network and rejoins when the connection drops.
  2.4 GHz only.
- **Data mode** is 8-bit clean, with no telnet processing. Toward the Mac, a 16 KB buffer holds what the
  network sends faster than the Plus reads it.
- **Settings** are kept in the ESP32's flash (NVS) and survive reflashing: speed, flow, the E/V/Q
  profile, networks, aliases, keys, addresses, models, PIN. The `+` settings are saved at once; E, V
  and Q are saved with `AT&W`.

## The AI relay (`ATDT ai`)

It implements all of `askai/relay/PROTOCOL.md`, ported from `askai/relay/relay.py`:

- the `ASKAI 1 Plus Modem relay` greeting, `HELLO` (with the PIN if one is set), `MODELS`, `USE`,
  `SYSTEM`, `NEW`, `MORE`, `ASK`, `STOP`, `PING` and `BYE`;
- streamed `BEGIN`, `T`, `N` and `END` lines, with `NOTE`, `ERR` and `BYE` lines as the protocol defines.

The model's Markdown becomes plain Mac Roman text exactly as the Python relay makes it.
`textconv.cpp` is a port of `askai/relay/textconv.py`, checked byte for byte against it. Headings lose
their `#`, lists get `•`, code blocks are indented, curly quotes and dashes come through, and emoji are
dropped.

| Model id (Model menu) | Provider / model |
| --- | --- |
| claude (default) | anthropic / claude-opus-5-5 |
| claude-sonnet | anthropic / claude-sonnet-5-5 |
| claude-haiku | anthropic / claude-haiku-5-5 |
| gpt, gpt-mini | openai / gpt-5, gpt-5-mini |
| gemini | gemini / gemini-2.5-flash |
| llama | openrouter / meta-llama/llama-3.3-70b-instruct |
| local | local / llama3.2 (once `AT+URL local=...` is set) |
| mock | the free mock model (canned replies; `!long`, `!slow`, `!error`, `!refuse`, `!cut`, `!key` in a message test those cases) |

Only models whose provider has a key (or, for local, an address) are offered.

- **Anthropic:** Messages API, `anthropic-version: 2026-10-21`. Opus 5.5 and Sonnet 5.5 get
  `fallbacks: "default"` with `anthropic-beta: server-side-fallback-2026-07-01`. No `temperature`,
  `top_p`, `top_k` or `thinking` is sent, and only `text_delta` text is read. A
  `stop_reason: "refusal"` ends the reply as `END refused`, with a note when nothing was written.
- **OpenAI and compatible servers:** Chat Completions.
- **Gemini:** streamGenerateContent.

All three stream over SSE. Neither the request nor the response is ever held whole in memory.

**Limits:**

- a message is at most 2,000 characters;
- a reply is at most 1,024 tokens;
- the conversation keeps up to 16,000 characters, dropping the oldest turns first with a note;
- 8 replies a minute;
- 400 replies a day;
- one relay connection at a time;
- a dropped session can be picked up again for 60 minutes (two sessions are kept).

**HTTPS:** certificates are checked against a CA bundle (`firmware/plusmodem/ca_bundle.h`). It holds the
Mozilla roots plus two legacy cross-signing roots that macOS still trusts: GlobalSign Root CA and AAA
Certificate Services. The second pair is needed because this core's verifier looks only at the issuer
of the top certificate a server sends. Anthropic, OpenAI, OpenRouter and Google all send GTS Root R4
cross-signed by GlobalSign Root CA, which Mozilla dropped in 2026. Regenerate the bundle with
`python3 modem/tools/gen_ca_bundle.py`.

## Security notes

- **API keys** reach the ESP32 only from the USB console (`setup.py key` reads them at a hidden prompt
  or from an environment variable). They are only ever shown masked. The echo of a typed key or
  password is `*`, and the monitor masks them too.
- **Stored in flash.** Keys and Wi-Fi passwords are stored unencrypted in the ESP32's flash. Anyone who
  has the board and a USB cable could read them out. Clear a key with `setup.py key <provider>` and an
  empty entry, or `AT+KEY provider=`.
- **eduroam's certificate.** The ESP32 doesn't check the eduroam server's certificate, which is common
  for simple devices. A fake "eduroam" access point nearby could capture a hash of the eduroam password.
- **Spending.** The relay's caps of 8 replies a minute and 400 a day limit what a stranger at the Plus
  could spend. A PIN (`AT+PIN=`) stops them from chatting at all.

## Build, flash and test

- `tools/toolchain.sh` installs the toolchain in `modem/build/`; `tools/flash.sh` runs it on first use.
  - Arduino core for ESP32 **2.0.17** (ESP-IDF 4.4), pinned with SHA-256 sums from Espressif's index.
  - Laid out as arduino-esp32's documented "hardware folder", with only the ESP32 compiler and esptool.
    `arduino-cli core install` would also fetch the RISC-V and S2/S3 compilers, about 3.5 GB more.
  - `arduino-cli` comes from Homebrew.
- The board is "ESP32 Dev Module" with the 1.9 MB app partition (minimal SPIFFS) and DIO flash. The
  firmware is about 1.07 MB.
- The first upload may need the board's **BOOT** button held while esptool connects, if the automatic
  reset doesn't work.
- `tests/run_host_tests.sh` runs the host tests (about a minute, no hardware, only 127.0.0.1):
  - the C++ core under AddressSanitizer: Hayes lines, `+++`, the relay with the mock and recorded
    Anthropic/OpenAI/Gemini streams, JSON, HTTP, SSE and settings;
  - the converter against the Python original;
  - an end-to-end run of `tests/sim.cpp`. It is the same core on TCP sockets
    (`socket://127.0.0.1:2324` works with every tool), driven by `relaytest.py` and console commands
    against a fake model API (`tests/mock_api.py`) and askai's own Python relay.

| Path | What it is |
| --- | --- |
| `firmware/plusmodem/` | the firmware: `modem.cpp` (Hayes lines, AT commands), `relay.cpp` (AI relay), `apis.cpp` (providers), `textconv.cpp` (Markdown to Mac Roman), `http.cpp`, `json.cpp`, `config.cpp`; `board.cpp` (ESP32: UART, pins, Wi-Fi, NVS, sockets, self-tests) |
| `tools/` | `flash.sh`, `toolchain.sh`, `console.py`, `setup.py`, `relaytest.py`, `pmlink.py` (shared), `gen_ca_bundle.py`, `gen_macroman.py` |
| `tests/` | `run_host_tests.sh`, `host_tests.cpp`, `test_textconv.py`, `tc_cli.cpp`, `sim.cpp`, `test_e2e.py`, `mock_api.py` |
