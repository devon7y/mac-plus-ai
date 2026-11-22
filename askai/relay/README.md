# Ask AI relay

The relay sits between the Mac and the model APIs. The Mac can't do HTTPS, so it talks plain text to
the relay over its modem port and a Wi-Fi modem; the relay talks HTTPS to Anthropic, OpenAI, Google
Gemini, OpenRouter or a local model server, holds the API keys, keeps each conversation, and turns
the model's Markdown into plain Mac Roman text.

```
Mac (modem port) -> serial cable -> Wi-Fi modem -> TCP -> relay -> HTTPS -> model API
```

| File | What it is |
| --- | --- |
| `relay.py` | the server (Python 3.8 or later, standard library only) |
| `providers.py` | the provider adapters: mock, Anthropic, OpenAI, OpenAI-compatible (OpenRouter, Ollama, LM Studio), Gemini |
| `textconv.py` | Markdown to plain text while streaming; Unicode to Mac Roman |
| `minitoml.py` | a small TOML reader for Pythons older than 3.11 |
| `relay.example.toml` | example configuration: providers, models, limits |
| `PROTOCOL.md` | the line protocol between the Mac and the relay |
| `tests/test_relay.py` | offline tests (about 8 s): `python3 tests/test_relay.py` |

## Run it

On any computer on the same network as the Wi-Fi modem that stays on while you chat: for example
an always-on desktop or a laptop.

```sh
mkdir -p ~/.askai
cp askai/relay/relay.example.toml ~/.askai/relay.toml      # then edit models and limits to taste
export ANTHROPIC_API_KEY=...        # only the providers you use; models without a key are hidden
export OPENAI_API_KEY=...
export GEMINI_API_KEY=...
export OPENROUTER_API_KEY=...
export ASKAI_PIN=2468               # any digits or letters; the Mac sends it (Connection > Settings)
python3 askai/relay/relay.py --check     # which models are usable (keys show only as "set"/"not set")
python3 askai/relay/relay.py             # listens on port 6800 on every interface
```

On Windows (Command Prompt) use `set ANTHROPIC_API_KEY=...` and `set ASKAI_PIN=2468` instead of
`export`, copy the example to `%USERPROFILE%\.askai\relay.toml`, and start it with
`py askai\relay\relay.py` (PowerShell: `$env:ANTHROPIC_API_KEY = "..."`).

The relay prints one line per connection, message and reply (lengths only; set `log_text = true` in
the config to log the text). Stop it with Control-C.

- **Address for the Mac:** the computer's LAN address and the port, for example `192.168.1.20:6800`
  (macOS: System Settings > Wi-Fi > Details; Windows: `ipconfig`; Linux: `hostname -I`). Give the
  computer a fixed address in the router (a DHCP reservation) so it doesn't change.
- **Firewall:** allow incoming TCP on port 6800 (macOS asks the first time; on Windows allow Python
  in "Windows Defender Firewall" for private networks).
- **Keep it running:** on the PC, a scheduled task at log-on or a `systemd --user` service
  (Linux) or `launchd` agent (macOS) that runs the command above with the variables set. Put the
  `export` lines in a file only you can read (for example `~/.askai/env`, `chmod 600`) and source it.
- **No internet exposure needed:** the modem and the relay are on the same Wi-Fi. If you ever forward
  the port from the internet, keep a long PIN; the relay locks out an address after 10 wrong PINs
  for an hour, and caps replies (8 a minute, 400 a day by default) so a stranger can't run up a bill.

## Configuration (`relay.toml`)

The relay reads, in order: `--config FILE`, `$ASKAI_CONFIG`, `~/.askai/relay.toml`,
`askai/relay/relay.toml`, then `relay.example.toml`. Keep your own copy outside the repository.

- `[relay]`: name, host, port, PIN (better from `ASKAI_PIN`), default model, and the limits:
  `max_reply_tokens` (1024), `max_turn_chars` (2000), `max_history_chars` (24,000; older turns are
  dropped), `replies_per_minute` (8), `replies_per_day` (400), `max_connections` (4),
  `idle_minutes` (30), and `system_prompt` (used when the Mac sends none).
- `[provider.NAME]`: `type` = `anthropic`, `openai`, `gemini`, `openai_compatible` or `mock`;
  `api_key_env` = the name of the environment variable with the key; optional `base_url`, `timeout`
  (seconds, default 120), `headers` (extra HTTP headers), and for `openai_compatible`
  `max_tokens_field` (`max_tokens` by default).
- `[[model]]`: `id` (what the Mac remembers; no blanks), `label` (the Mac's Model menu), `tag` (the
  name in front of each reply, at most 8 letters), `provider`, `model` (the provider's model name),
  and optionally `effort` (Anthropic and OpenAI reasoning effort, e.g. `"low"` for faster first
  words), `fallbacks = "default"` (Anthropic: when a safety classifier declines, Anthropic retries on
  a suitable model), and `extra_body` (fields added to the request as they are).

Model names change over time: check each provider's model list and edit the `[[model]]` entries. The
example lists Claude Opus 5.5, Sonnet 5.5 and Haiku 5.5, GPT-5 and GPT-5 mini, Gemini 2.5 Flash, a
Llama model on OpenRouter and the free mock model.

### A local model (no key, no cost)

Ollama: `ollama serve`, `ollama pull llama3.2`, then in `relay.toml`:

```toml
[provider.local]
type = "openai_compatible"
base_url = "http://localhost:11434/v1"

[[model]]
id = "local"
label = "Llama 3.2 (on this computer)"
tag = "Llama"
provider = "local"
model = "llama3.2"
```

LM Studio works the same way with `base_url = "http://localhost:1234/v1"`.

## Testing without the Mac

- `python3 askai/relay/relay.py --mock-only` offers only the mock models (canned replies, no
  network, no keys). Words in a message trigger its test cases: `!error`, `!long`, `!slow`, `!cut`,
  `!refuse`, `!key`.
- Talk to it by hand: `nc 127.0.0.1 6800`, then type `HELLO 1 -` (or your PIN), `MODELS`,
  `ASK hello` (lines end with Return).
- `askai/modem/hayes_emu.py` pretends to be the Wi-Fi modem, and `askai/tests/run_chain.sh` runs
  the whole chain with the Mac app in MAME.

## How it behaves

- Each connection gets a session (an 8-digit id). If the line drops, the Mac reconnects with the
  same id within an hour and the conversation continues.
- Replies stream as the model writes them, at most 0.12 s apart, in lines of at most 200 bytes.
- Errors from a provider (bad key, rate limit, overloaded, unknown model, network) reach the Mac
  as a one-line explanation; the failed turn isn't kept.
- Nothing about a key is ever sent to the Mac, logged or written to disk.
