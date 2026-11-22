# Ask AI line protocol, version 1

How the Mac app and the relay talk. The connection is any byte stream: TCP to the relay, which on the
Mac's side is the modem port, a Hayes WiFi modem in data mode (after `CONNECT`) and the Wi-Fi network.

```
Mac (modem port) -> serial cable -> WiFi modem -> TCP -> relay -> HTTPS -> model API
```

## Framing

- One message per **line**. A line ends with **CR** (13). LF (10) also ends a line, so CRLF works; empty
  lines are ignored. The relay always ends its lines with a single CR.
- Text is **Mac Roman** both ways. The relay converts to and from UTF-8.
- A line starts with an upper-case keyword; arguments follow after one blank.
- Lines are at most 2,048 bytes (the relay answers `ERR TOOLONG` to a longer one and skips it).
  Reply text lines from the relay are at most 203 bytes (`T ` + 200 + CR), so the Mac needs only a
  small line buffer.

## Opening a session

| Direction | Line | Meaning |
| --- | --- | --- |
| relay → Mac | `ASKAI 1 <relay name>` | Sent at once on connect: protocol version 1. The Mac scans for it among the modem's own lines. |
| Mac → relay | `HELLO 1 <pin> [<session>]` | Version, PIN (`-` for none), and optionally the id of an earlier session to pick up after the line dropped. Must come first (within 2 minutes). |
| relay → Mac | `OK HELLO <session> NEW\|RESUMED <turns> <model id>` | Session id (8 hex digits), whether the old conversation was kept, how many turns it holds, and the current model (`-` if no model is usable). |
| relay → Mac | `ERR PIN Wrong PIN.` | Try again; after 3 wrong PINs: `BYE PIN ...` and the relay hangs up. |

A session is kept for 60 minutes after the line drops; `HELLO` with its id (and the right PIN)
resumes it with the conversation intact.

## Commands (Mac → relay) and answers

| Mac sends | Relay answers |
| --- | --- |
| `MODELS` | one `MODEL <id> <tag> <label>` per usable model, then `OK MODELS <count> <current id>`. `id` has no blanks (at most 31 characters); `tag` is the short speaker name for the transcript (at most 8, no blanks); `label` is the menu text (at most 40). |
| `USE <id>` | `OK USE <id> <tag> <label>` or `ERR MODEL ...` |
| `SYSTEM <text>` | `OK SYSTEM`. Sets this session's system prompt; empty text means the relay's default. |
| `NEW` | `OK NEW`. Forgets the conversation (the model and system prompt stay). |
| `MORE <text>` | `OK MORE`. One more line of a multi-line message; `ASK` sends it. |
| `ASK <text>` | Sends a user turn (with any `MORE` lines before it). The reply streams back (below). |
| `STOP` | Ends the current reply early (its `END` says `stopped`); `OK STOP` when nothing was streaming. |
| `PING` | `PONG` |
| `BYE` | `OK BYE`, then the relay hangs up (the modem then says `NO CARRIER`). |

While a reply streams the Mac may only send `STOP` and `PING` (anything else gets `ERR BUSY`).

## A streamed reply

```
BEGIN <model id> <tag>        the reply starts
T <text>                      text to append (no line breaks inside; may end in a blank)
N                             a line break (two in a row: a paragraph break)
...                           more T / N lines as the model writes
END <stop> <tokens> <bytes>   the reply is over
```

- `stop` is `done`, `length` (hit the token limit), `stopped` (the Mac sent STOP), or `refused` (the
  model or its safety system declined; a `NOTE` explains).
- `tokens`: output tokens the provider reported (0 if unknown); `bytes`: reply bytes sent (T and N
  lines with their CRs).
- Text arrives roughly word by word. The relay has already turned the model's Markdown into plain
  text: headings without `#`, `•` bullets, code blocks indented by four blanks, no `*`/`_`/backticks,
  links as `text (url)`, tables as `a | b` rows, emoji removed, curly quotes and dashes in Mac Roman.
  The relay never wraps lines: the Mac does, at word boundaries, to its own window width. Blanks at
  the start of a line after `N` are indentation and should be kept.
- If the provider fails, the reply ends with `ERR <code> <message>` instead of `END`. Text already
  shown stays; the turn is not kept in the conversation.

## Other lines from the relay

| Line | Meaning |
| --- | --- |
| `NOTE <text>` | Information to show as a note, e.g. "Older messages were forgotten to make room." |
| `ERR <code> <message>` | An error. Codes: `PIN` wrong PIN, `PROTO` bad or out-of-order command, `TOOLONG`, `BUSY` a reply is streaming, `MODEL` unknown model or none usable, `LIMIT` rate or daily limit (relay or provider), `KEY` API key missing or rejected, `NET` the relay can't reach the provider, `PROVIDER` any other provider error, `INTERNAL` a relay bug. The message is meant for the user. |
| `BYE <reason> <text>` | The relay is about to hang up: `IDLE` (no HELLO within 2 minutes, or 30 idle minutes), `BUSY` (too many connections), `PIN`, `LOCKED` (too many wrong PINs from this address). |

## Limits (relay defaults, set in relay.toml)

| Limit | Default |
| --- | --- |
| Message length (`ASK` + `MORE`) | 2,000 characters |
| Reply length | 1,024 tokens |
| Conversation kept | 24,000 characters (oldest turns dropped first, with a NOTE) |
| Replies | 8 per minute per session, 400 per day for the relay |
| Connections | 4 at once |
| Wrong PINs | 3 per connection; 10 per address lock it out for 60 minutes |

## Example

```
relay: ASKAI 1 Ask AI relay
Mac:   HELLO 1 2468
relay: OK HELLO 9F3A01C2 NEW 0 claude
Mac:   MODELS
relay: MODEL claude Claude Claude Opus 5.5
relay: MODEL gpt GPT OpenAI GPT-5
relay: OK MODELS 2 claude
Mac:   SYSTEM Answer in short plain text.
relay: OK SYSTEM
Mac:   ASK What year did the Mac Plus come out?
relay: BEGIN claude Claude
relay: T The Macintosh Plus came out in
relay: T January 1986.
relay: END done 12 49
```

## Flow control

Replies are small (at most about 4 KB at the default token limit), so the Mac gives the serial driver a
16 KB input buffer and needs no flow control of its own; hardware or XON/XOFF handshaking between the
Mac and the modem is optional (a Settings choice). The relay writes as fast as TCP lets it; the modem
paces bytes to the serial speed.
