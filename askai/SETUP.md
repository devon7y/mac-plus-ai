# Ask AI: setting it up

Ask AI lets you chat with today's AI models (Claude, GPT, Gemini and others) from the Mac Plus or the
SE. The Mac talks through its modem port to your Wi-Fi modem, which connects over Wi-Fi to the
**Ask AI relay**, a small program on a computer that stays on. The relay holds the API keys and talks
to the model services.

```
Mac Plus (modem port) -> modem cable -> Wi-Fi modem -> Wi-Fi -> relay (a computer) -> internet -> the model
```

## What you need

| Thing | Notes |
| --- | --- |
| The Ask AI floppy | `askai_plus.img` (800K, Mac Plus, 1 MB is enough) or `askai_se.img` (1.44 MB, the SE). Each starts straight into Ask AI. |
| A Macintosh **modem cable** | Mini-DIN-8 at the Mac end. At the other end whatever your Wi-Fi modem has: most have a DB-25 (a "Mac to modem" cable, mini-DIN-8 to DB-25 male); for a DB-9 modem use a mini-DIN-8 to DB-9 modem cable or a DB-25/DB-9 adapter. Not a printer or null-modem cable. |
| The Wi-Fi modem | Any Hayes-compatible "Wi-Fi modem" (Zimodem/WiModem232, WiFi232, ESP8266 "virtual modem" firmware). It needs power (usually USB). |
| A computer for the relay | Any computer that stays on, on the same Wi-Fi network. Python 3.8 or later. |

The cable wiring, if you ever need to check or make one (Mac mini-DIN-8 to DB-25):

| Mac pin | Signal | DB-25 pin |
| --- | --- | --- |
| 1 | HSKo (handshake out) | 20 (DTR), and 4 (RTS) |
| 2 | HSKi (handshake in) | 5 (CTS) on "hardware handshaking" cables; some cables use 8 (DCD) |
| 3 | TxD- | 2 (TxD) |
| 4 | Ground | 7 (Ground) |
| 5 | RxD- | 3 (RxD) |
| 8 | RxD+ | 7 (Ground) |
| 6, 7 | TxD+, GPi | not connected (Ask AI doesn't use them) |

## 1. Start the relay

On that computer, once (the details are in `askai/relay/README.md`):

```sh
mkdir -p ~/.askai
cp askai/relay/relay.example.toml ~/.askai/relay.toml
export ANTHROPIC_API_KEY=...      # and/or OPENAI_API_KEY, GEMINI_API_KEY, OPENROUTER_API_KEY
export ASKAI_PIN=2468             # pick your own; the Mac must send the same one
python3 askai/relay/relay.py --check     # lists the models it can offer
python3 askai/relay/relay.py
```

Note the computer's address on your network (for example `192.168.1.20`) and keep the relay running.
The relay's port is `6800`. Allow it through the computer's firewall if asked. Without any key the
relay still offers the free **Mock Model**, handy for a first test of the cable and the modem.

## 2. Plug in

1. Mac switched off. Plug the modem cable into the **modem port** (the port with the telephone icon,
   at the back) and into the Wi-Fi modem. Power the modem.
2. Put the Ask AI floppy in and switch the Mac on. Ask AI starts and says "Welcome to Ask AI!".

## 3. Set up the Wi-Fi modem (once)

Ask AI has a modem terminal for this: **Connection > Modem Terminal**. Each line you type goes to the
modem when you press Return, and the modem's answers appear below it.

1. Choose **Connection > Settings…**, set **Speed** to the modem's present speed (new modems are
   often at 1200, 9600 or 115200; Ask AI offers 1200 to 57600, so try 9600 or 1200 first if you
   don't know), **Handshake: None**, **Port: Modem**. Click OK. (A modem set to 115200 is too fast
   for the Plus: set it to 19200 once from a modern computer with a USB-serial adapter, or use the
   modem's own reset-to-defaults if its manual has one.)
2. **Connection > Modem Terminal**. Type `AT` and press Return. The modem should answer `OK`. If
   nothing comes back, try another speed in Settings (choose Connection > Disconnect first). Some
   modems (WiFi232) find the speed by themselves from the first `AT`.
3. Join your Wi-Fi network and set the speed to **19200**, then save. The commands depend on the
   modem's firmware (type `ATI` to see which one you have; its manual lists them all):

   | Firmware | Join Wi-Fi | Set 19200 baud and save |
   | --- | --- | --- |
   | Zimodem (WiModem232 and many ESP modems) | `ATW"YourNetwork,YourPassword"` (or the menu: `AT+CONFIG`) | `ATB19200` then `AT&W` |
   | WiFi232 / ESP8266 "virtual modem" | `AT$SSID=YourNetwork`, `AT$PASS=YourPassword`, `ATC1` | `AT$SB=19200` then `AT&W` |

4. The modem switches speed at once, so it stops understanding the Mac: choose **Connection >
   Disconnect**, set **Speed: 19200** in Settings, then Modem Terminal again and type `AT`: `OK`.
   (19200 is the default and is plenty: replies arrive far faster than you can read them.)
5. Optional test: `ATDT 192.168.1.20:6800` (your relay's address). The modem says `CONNECT` and the
   relay answers `ASKAI 1 Ask AI relay`. Type `BYE` and the relay hangs up (`NO CARRIER`).
6. **Connection > Disconnect** to leave the terminal.

## 4. Tell Ask AI where the relay is

**Connection > Settings…**:

| Setting | What to enter |
| --- | --- |
| Relay address | the relay computer's address and port, e.g. `192.168.1.20:6800` (Ask AI dials `ATDT` + this; a line starting with `AT` is sent exactly as typed) |
| Modem init string | `ATE0V1` (echo off, word answers). Leave as it is unless your modem needs more |
| PIN | the same as `ASKAI_PIN` on the relay (empty if the relay has none) |
| Speed | 19200 (must match the modem) |
| Handshake | None. "Hardware (CTS)" only with a hardware-handshaking cable; XON/XOFF only if the modem is set for it |
| Port | Modem |
| Connect when Ask AI starts | on: it dials as soon as the floppy starts |
| Instructions for the model | what every model is told first (keep answers short and plain for this screen); empty uses the relay's own |

Click OK. The settings are saved on the floppy ("Ask AI Prefs" in the System Folder), so keep the
floppy unlocked.

## 5. Chat

1. **Connection > Connect** (⌘K). The status at the top right shows "waking the modem…",
   "dialling…", "connecting…", then the model's name.
2. Type a message and press **Return**. The reply appears word by word; the status shows the speed
   in bytes per second. You can type your next message while a reply is still coming: Return queues
   it and it goes as soon as the reply ends.
3. **Model** menu: pick another model (the relay's list; the check mark shows the current one). Ask
   AI remembers your choice.
4. **File > New Chat** (⌘N): the model forgets the conversation. **⌘.** stops a reply.
5. The scroll bar on the right (or the up and down arrow keys; Option for a page) scrolls back
   through the conversation.
6. **Connection > Disconnect** (⌘K) hangs up. Quitting hangs up too.

If the line drops in the middle, connect again: within an hour the relay picks up the same
conversation.

## When something goes wrong

| Ask AI says | What to do |
| --- | --- |
| The modem doesn't answer… | Modem on? Cable in the **modem** port? Speed in Settings the same as the modem's? Try Modem Terminal and `AT`. |
| The modem couldn't reach the relay at … (NO CARRIER) | Is the relay running? Right address and port? Firewall? The relay computer on the same network? |
| The modem isn't on Wi-Fi (NO DIALTONE) | Join the network again (step 3). |
| The modem rejected the init string / dial command (ERROR) | Check those two settings; try init string `AT` alone. |
| Connected, but no Ask AI relay answered | Something else answers at that address: check the port (6800). |
| The relay didn't accept the PIN | Same PIN in Settings as `ASKAI_PIN` on the relay. |
| No API key: set … where the relay runs | Set that key on the relay computer and restart the relay, or pick another model. |
| The provider's rate limit was hit / is busy | Wait a minute and ask again. |
| The connection to the relay was lost | Wi-Fi or the relay went away: Connection > Connect again. |
| Ask AI couldn't open the modem port | Another program has the port, or (printer port only) AppleTalk is on: turn it off in the Chooser. |
| Settings couldn't be saved | The floppy is locked: slide the tab to unlock it. |

Ask AI by itself never needs internet access on the Mac and never sees your API keys; they stay on
the relay computer.
