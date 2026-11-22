# Ask AI: chat with today's AI models from a 1986 Macintosh

Ask AI is a chat app for a Macintosh Plus or SE. You type on the Mac, and a modern model answers on
its screen word by word: Claude, GPT, Gemini, any model on OpenRouter, or one running on your own
computer. The Mac can't do HTTPS, so it talks plain text through its modem port to a Hayes-compatible
Wi-Fi modem. The modem opens a TCP connection to the Ask AI relay, a small Python program on a
computer on the same network. The relay holds the API keys, calls the model's API over HTTPS, and
sends back plain Mac Roman text.

```text
Mac (modem port) -> modem cable -> Wi-Fi modem -> TCP -> relay -> HTTPS -> model API
```

**Status:** the whole chain has run in MAME, from the app through an emulated modem to the relay and
its mock model. It has not yet been tried on a real Mac with a real cable and modem, or against the
real provider APIs. See [What has been tested](#what-has-been-tested).

Ask AI is not on the website at [macintosh.devon7y.com](https://macintosh.devon7y.com): it needs a
modem, and the browser emulator has no serial port.

## Two ways to connect

1. **The Python relay** in [`relay/`](relay/), on any computer on the same network as the modem, with
   any Hayes-compatible Wi-Fi modem (the Zimodem / WiModem232, WiFi232 and ESP8266 "virtual modem"
   families). The relay address in Ask AI is that computer's address and port, for example
   `192.168.1.20:6800`.
2. **The [Plus Modem](../modem/README.md)**, the ESP32 Wi-Fi modem firmware in this repository. It
   has the same relay built in, so no other computer is needed: set the relay address to `ai`. Its
   README covers the hardware, how keys are stored on the board, and its own limits.

[`SETUP.md`](SETUP.md) walks through the first option step by step: the cable and its wiring, setting
up the Wi-Fi modem, starting the relay, the first connection, and what each error message means.

## Using it

| Action | How |
| --- | --- |
| Send a message | Type it, Return (queued while a reply streams) |
| Correct a typo | Delete |
| Stop a reply | ⌘. or **Connection > Stop Reply** |
| Connect / hang up | **Connection > Connect / Disconnect** (⌘K) |
| Pick a model | **Model** menu |
| Start over | **File > New Chat** (⌘N) |
| Scroll back | Scroll bar, or the up/down arrow keys (Option for a page) |
| Relay address, PIN, speed, system prompt | **Connection > Settings…** |
| Talk to the modem directly | **Connection > Modem Terminal** |
| About / Quit | Apple menu / **File > Quit** (⌘Q) |

The window fills the screen: a header with a status line (dialling, then the model's name and bytes
per second while a reply arrives), the transcript with "You" and model labels, and an input line.
Replies stream in word by word, and you can type your next message meanwhile. The Model menu is
built from the relay's list, with a check mark on the current model. The Edit menu works on the
input line.

**Settings** (saved in "Ask AI Prefs" in the System Folder, so keep the floppy unlocked):

| Setting | Notes |
| --- | --- |
| Relay address | Ask AI dials `ATDT` followed by this. A line starting with `AT` is sent as typed. `DIRECT` skips the modem (a plain serial line to the relay). |
| Modem init string | default `ATE0V1` (echo off, word results) |
| PIN | must match the relay's PIN |
| Speed | 1200 to 57600 baud, default 19200. Must match the modem. |
| Handshake | None, XON/XOFF, or Hardware (CTS) |
| Port | Modem or Printer |
| Connect when Ask AI starts | dials as soon as the app opens |
| Instructions for the model | a system prompt of up to 255 characters, empty for the relay's own |

**Modem Terminal** sends each line you type straight to the modem and shows its answers. It is there
so you can join Wi-Fi and set the modem's speed from the Mac, with no other software.

## How it works

### The Mac app

The app is C built with [Retro68](https://github.com/autc04/Retro68), in [`app/`](app/). Its chat
layout comes from [Pixel](../llm/), the chatbot that runs on the Plus itself. The boot floppies use
System 6, and the app also starts under System 7 on an SE.

- **Serial.** The app drives the Serial Driver on the modem port (`.AIn` / `.AOut`) through Device
  Manager parameter blocks. It gives the driver a 16 KB input buffer, keeps one asynchronous read
  always pending, and queues asynchronous writes, so the screen stays responsive while bytes arrive.
- **Dialling.** It sends `AT` until the modem answers `OK`. If there is no answer, the modem may still
  be online from last time, so it waits the guard time, sends `+++` and `ATH`, and tries again. Then
  it sends the init string and `ATDT` with the relay address, and waits up to 50 s for `CONNECT`. It
  tolerates echo and numeric result codes. `NO CARRIER` at any time means the line is gone.
- **Hanging up.** It sends `BYE`, and the relay closes the connection. If that fails it uses guard
  time, `+++` and `ATH`. Then it closes the port, which also drops DTR.
- **Resuming.** After a dropped line, connecting again within an hour picks up the same conversation.
- **Prefs.** "Ask AI Prefs" (464 bytes) is written only when the line is idle, because floppy access
  masks interrupts.

### The line protocol

The Mac and the relay exchange lines of Mac Roman text ending in CR. The full protocol is in
[`relay/PROTOCOL.md`](relay/PROTOCOL.md). A short session:

```text
relay: ASKAI 1 Ask AI relay
Mac:   HELLO 1 YOUR_PIN
relay: OK HELLO 9F3A01C2 NEW 0 claude
Mac:   MODELS
relay: MODEL claude Claude Claude Opus 5.5
relay: MODEL gpt GPT OpenAI GPT-5
relay: OK MODELS 2 claude
Mac:   ASK What year did the Mac Plus come out?
relay: BEGIN claude Claude
relay: T The Macintosh Plus came out in
relay: T January 1986.
relay: END done 12 49
```

- The relay greets with `ASKAI 1`. The Mac looks for that line among the modem's own messages.
- `HELLO` carries the protocol version, the PIN (`-` for none) and, after a dropped line, the old
  session id.
- Other commands: `MODELS`, `USE <id>`, `SYSTEM <text>`, `NEW` (forget the conversation), `MORE`
  (one more line of a multi-line message), `ASK`, `STOP`, `PING` and `BYE`.
- A reply streams as `BEGIN`, then `T <text>` lines (text to append) and `N` lines (line breaks), then
  `END <stop> <tokens> <bytes>`. `NOTE` lines carry information and `ERR <code> <message>` lines carry
  errors meant for the user.
- Reply lines are at most 203 bytes, so the Mac needs only a small line buffer. The relay never
  wraps text. The Mac wraps it to its own window width.

There is no flow control of its own. A reply at the default limit is at most about 4 KB, which fits
in the Mac's 16 KB serial buffer, and the modem paces bytes to the serial speed.

### The relay

[`relay/relay.py`](relay/relay.py) is an asyncio TCP server for Python 3.8 or later, using only the
standard library. It reads its TOML config with `tomllib`, or with the bundled
[`minitoml.py`](relay/minitoml.py) on Pythons older than 3.11.

- **Sessions.** Each connection gets a session that keeps the conversation, so the Mac sends only the
  new turn. A session survives a dropped line for 60 minutes. History beyond 24,000 characters is
  dropped, oldest first, with a note to the Mac.
- **Plain text.** [`relay/textconv.py`](relay/textconv.py) turns the model's Markdown into plain text
  while it streams. Headings lose their `#`, lists get `•` bullets, code blocks are indented four
  spaces, emphasis markers and backticks go, links become `text (url)`, tables become `a | b` rows and
  emoji are dropped. Unicode is mapped to Mac Roman, with readable fallbacks for characters Mac Roman
  lacks. Curly quotes and dashes come through.
- **Streaming.** Reply text goes out at least every 0.12 s, in lines of at most 200 bytes.
- **HTTPS** requests run in worker threads (`urllib`), so a slow provider never blocks other
  connections.
- **Errors** from a provider (bad key, unknown model, rate limit, overload, network) reach the Mac as
  a one-line message with a code: `KEY`, `MODEL`, `LIMIT`, `PROVIDER` or `NET`. The failed turn isn't
  kept.

[`relay/README.md`](relay/README.md) has the full guide: running it, keeping it running, the firewall,
every config field, and a local model with Ollama or LM Studio.

### Supported providers

Each provider in `relay.toml` has a `type`:

| `type` | API | Notes |
| --- | --- | --- |
| `anthropic` | Anthropic Messages API, streamed | `x-api-key` and `anthropic-version: 2026-10-21`. Optional `effort`. `fallbacks = "default"` (with its beta header) lets Anthropic retry on another model when a safety classifier declines. No `temperature` is sent. |
| `openai` | OpenAI Chat Completions, streamed | Bearer token, `max_completion_tokens` |
| `openai_compatible` | the same wire format | OpenRouter, or a local server such as Ollama or LM Studio (no key needed) |
| `gemini` | Google Gemini `streamGenerateContent` (SSE) | `x-goog-api-key`. Thought parts are skipped. |
| `mock` | none | canned Markdown replies with realistic timing, free, no network |

The example config, [`relay/relay.example.toml`](relay/relay.example.toml), lists Claude Opus 5.5,
Sonnet 5.5 and Haiku 5.5, GPT-5 and GPT-5 mini, Gemini 2.5 Flash, Llama 3.3 70B on OpenRouter, and the
mock model. Model names change, so check them against each provider's current list. The mock model
tests error paths when a message contains `!error`, `!long`, `!slow`, `!cut`, `!refuse` or `!key`.

### API keys

- Keys come only from **environment variables**. Each provider in the config names its variable
  (`api_key_env`, for example `ANTHROPIC_API_KEY`), and the relay reads it when it makes a request.
  Keys are never written in the config file.
- A key is never sent to the Mac, logged or written to disk. `relay.py --check` lists the models and
  shows each key only as "set" or "not set". Models whose provider has no key are hidden from the Mac.
- Keep your config outside the repository. The relay looks for `--config FILE`, then `$ASKAI_CONFIG`,
  then `~/.askai/relay.toml`, then `relay/relay.toml`, then the example. Set the PIN with `ASKAI_PIN`
  rather than in the file.
- The tests never touch real keys. `tests/run_chain.sh` starts the relay with an empty environment,
  and the adapter tests use test-only variable names.
- With the Plus Modem's built-in relay, keys live on the ESP32 instead. Its
  [README](../modem/README.md#security-notes) explains how they get there and the risks.

### Security notes

- **PIN.** Set one. The relay compares it in constant time (`hmac.compare_digest`) and waits 1 s after
  each wrong PIN. A connection gets 3 tries, and 10 wrong PINs from one address lock that address out
  for 60 minutes. The relay prints a warning at start if it has no PIN and listens beyond localhost.
- **Plain text on your network.** The link between the modem and the relay is ordinary TCP with no
  encryption, so the PIN and the conversation cross your network in the clear. The relay listens on
  every interface (port 6800) by default. Keep it on your own network. If you ever forward the port
  from the internet, use a long PIN.
- **Cost limits** (defaults, set in `relay.toml`): 8 replies a minute per session, 400 replies a day
  for the whole relay, 1,024 tokens per reply, 2,000 characters per message, 4 connections at once,
  and a 30-minute idle timeout. A connection must send `HELLO` within 2 minutes.
- **Logs** record lengths only. Message text is logged only if you set `log_text = true`.
- **On the Mac,** the PIN is stored in plain form in "Ask AI Prefs" on the floppy. The Mac never sees
  an API key.

## What has been tested

Verified, on the host and in MAME:

- `relay/tests/test_relay.py`: 17 offline tests pass, also under Python 3.9. They cover the Markdown
  rendering (any chunking gives the same text), Mac Roman, and each adapter against a local fake
  server replaying handwritten streams in the providers' documented formats. The fake server checks
  the headers and request bodies. The tests also cover stop reasons, mid-stream errors, HTTP 400, 401,
  404, 429 and 529, a missing key, an unreachable server, and the protocol end to end (PIN, models,
  `STOP` mid-reply, `BUSY`, `TOOLONG`, rate limit, lockout, history trim, resuming after a drop).
- `modem/test_modem.py`: the Hayes modem emulator with the relay (echo, result codes, dialling, a
  chat turn, `+++` and `ATO`, `+++` inside data passing through, `ATH`, a failed dial, the relay
  hanging up).
- **The whole chain in MAME** (`tests/run_chain.sh`, an emulated 1 MB Mac Plus in real time): the test
  floppy dialled the relay through the modem emulator by itself and was ready at 20.4 s. Two messages
  got two streamed replies (389 and 362 bytes at about 180 bytes/s, the mock's pace) with bullets
  rendered and the scroll bar following, and no serial errors. A second script checked the Settings
  dialog, switching models, a provider error shown as a note, disconnecting, and the Modem Terminal
  (`ATI` answered). The saved prefs were read back from the disk afterwards.
- **The SE floppy** boots into Ask AI on an emulated SE with 4 MB. So does a System 7.0.1 SE hard disk
  with the app in Startup Items (316,852 bytes free in its 384K partition). The Plus release floppy is
  built the same way as the test floppy above, but wasn't booted on its own.

Not tested:

- Anything on real hardware: the Plus, the SE, the cable, a real Wi-Fi modem, and real serial speeds.
- The real provider APIs. The adapters were tested only against handwritten streams in each
  provider's documented format.
- On the Mac side: hanging up with `+++` and `ATH`, recovering a modem that is still online, XON/XOFF
  and hardware handshaking, the printer port, speeds other than the test speed, and serial under
  System 7 (only booting was checked).
- The relay on Windows, and the About box.

**MAME's serial clock is 6.7% fast.** MAME 0.289 drives the Plus's SCC baud generator from 3.9168 MHz,
where a real Plus uses 3.672 MHz. So in MAME every standard Mac rate comes out 6.7% fast and none meets
MAME's null modem (bytes arrive with bit 7 wrong). The app's rates are right for real hardware. For
tests it has a hidden speed, set only through the prefs file, that is exactly 7,200 baud in MAME (6,750
on a real Mac), and the test scripts set the null modem to 7,200. Two emulated Macs agree with each
other, so this only affects links to programs on the host.

## Building and running

You need [Retro68](https://github.com/autc04/Retro68) (see [`../toolchain/README.md`](../toolchain/README.md)),
CMake with Ninja, `hfsutils`, Python 3, and MAME for the chain test. Apple's System software isn't in
this repository. The disk builder expects the System 6.0 and 6.0.7 disk images listed in the
top-level README.

```sh
python3 -I askai/build.py
# -> askai/build/release/askai_plus.img   800K Mac Plus boot floppy (1 MB Plus, System 6.0 + Narrow Screen)
#    askai/build/release/askai_se.img     1.44 MB SE boot floppy (System 6.0.7)
#    askai/build/release/Ask AI.bin       the application with its Finder icon
```

The app is 27,264 bytes as a MacBinary file (21 KB of code) and leaves 775,980 bytes free after start
on a 1 MB Plus.

Run the relay with real keys (replace the placeholders):

```sh
mkdir -p ~/.askai
cp askai/relay/relay.example.toml ~/.askai/relay.toml   # edit models and limits to taste
export ANTHROPIC_API_KEY=YOUR_KEY     # and/or OPENAI_API_KEY, GEMINI_API_KEY, OPENROUTER_API_KEY
export ASKAI_PIN=YOUR_PIN
python3 askai/relay/relay.py --check  # which models are usable
python3 askai/relay/relay.py          # listens on port 6800
```

Without the Mac:

```sh
python3 askai/relay/relay.py --mock-only   # only the mock models: no keys, no network
nc 127.0.0.1 6800                          # then type: HELLO 1 -   MODELS   ASK hello
python3 askai/relay/tests/test_relay.py    # relay tests, about 10 s
python3 askai/modem/test_modem.py          # modem emulator tests, about 5 s
askai/tests/run_chain.sh                   # the whole chain in MAME: relay (mock) + modem emulator + Mac
```

`run_chain.sh` builds a test floppy whose prefs dial the relay, starts the mock relay and
[`modem/hayes_emu.py`](modem/hayes_emu.py), and connects MAME's modem port to the emulator with
`-modem null_modem -bitb socket.127.0.0.1:<port>`. `SCRIPT=m2.lua askai/tests/run_chain.sh` runs the
Settings and menus check.

## Files

| Path | What |
| --- | --- |
| [`app/`](app/) | the Mac app: `main.c` window, menus and settings, `link.c` dialling and the protocol, `serial.c` the Serial Driver, `askai.r` resources |
| [`build.py`](build.py) | builds the app, its icon and both boot floppies (`--test-dial` adds a test floppy) |
| [`relay/relay.py`](relay/relay.py) | the relay server |
| [`relay/providers.py`](relay/providers.py) | the provider adapters: mock, Anthropic, OpenAI, OpenAI-compatible, Gemini |
| [`relay/textconv.py`](relay/textconv.py) | Markdown to plain text while streaming, Unicode to Mac Roman |
| [`relay/minitoml.py`](relay/minitoml.py) | a small TOML reader for older Pythons |
| [`relay/relay.example.toml`](relay/relay.example.toml) | example config: providers, models, limits (key variable names only) |
| [`relay/PROTOCOL.md`](relay/PROTOCOL.md) | the line protocol |
| [`relay/README.md`](relay/README.md) | running and configuring the relay |
| [`relay/tests/`](relay/tests/) | offline relay tests |
| [`modem/hayes_emu.py`](modem/hayes_emu.py), `test_modem.py` | a Hayes Wi-Fi modem emulator for testing without hardware, and its test |
| [`tests/`](tests/) | the MAME chain test (`run_chain.sh`) and its Lua scripts |
| [`SETUP.md`](SETUP.md) | setting it up with real hardware, step by step |
| [`../toolchain/icons/art/askai.txt`](../toolchain/icons/art/askai.txt) | the Finder icon |

All code, text and the icon are original. Ask AI uses no training data or third-party assets.
