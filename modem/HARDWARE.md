# Wiring the Wi-Fi modem: Mac Plus ↔ ESP32

This is the hardware guide. Every connection that could harm something is checked before it's made.

```
Mac Plus modem port ─ mini-DIN-8 cable ─ DB9 screw breakout ─ MAX3232 module ─ ESP32 ─ Wi-Fi
   (the phone icon)    (8 bare wires)      (plugs into it)     (level shifter)  (the "modem")
```

The Plus's serial port swings about ±5 V. The ESP32 works at 3.3 V and would be damaged by those
voltages. The MAX3232 module converts between the two.

## What you need

- All the parts you have: the ESP32 board, the MAX3232 module, the DB9 male screw breakout, the
  mini-DIN-8 cable and the jumper kit.
- A **USB-C data cable** for the MacBook. Some USB-C cables only charge; if the ESP32 doesn't show up
  on the MacBook, try another cable.
- A USB charger and a USB-C cable to power the ESP32 next to the Plus.
- Electrical tape to insulate the wires you don't use.
- **A multimeter is strongly recommended** (a basic one is about $20). Steps 4 and 6 use it. Without
  one you can still finish, as the steps explain, but with less certainty.

## Step 1: plug the ESP32 into the MacBook

Use the USB-C data cable. Tell me when it's plugged in. I'll put the firmware on it from here; you
don't need to install anything.

## Step 2: connect the ESP32 to the MAX3232 module (4 jumper wires)

Both boards have male pins, so use **female-to-female** jumpers.

| MAX3232 module pin | ESP32 pin | Note |
| --- | --- | --- |
| VCC | **3V3** (also labelled 3.3V) | **Never 5V or VIN.** At 5 V the module would send 5 V into the ESP32 and damage it. |
| GND | GND | any GND pin |
| TX | GPIO16 (labelled RX2 or 16) | The two data wires can go either way round: |
| RX | GPIO17 (labelled TX2 or 17) | the firmware detects which is which when it starts. |

## Step 3: loopback test (checks the ESP32, the module and these 4 wires)

1. Push the DB9 breakout into the module's DB9 socket. It's only a friction fit, which is fine.
2. Put a short wire between breakout terminals **2** and **3**, so anything the module sends comes
   straight back.
3. With the ESP32 plugged into the MacBook, run `python3 modem/tools/console.py` and type `AT+LOOP`.
   It should answer **LOOPBACK OK**.
4. Remove the wire between 2 and 3.

## Step 4: find the module's output pin (multimeter)

Exactly one of DB9 pins 2 and 3 is the module's **output** (what it sends to the Mac). The other is
its **input**. It's usually pin 2 = output, pin 3 = input, but these modules vary, so measure:

- ESP32 powered and the module connected as in Step 2. Multimeter on DC volts.
- Black probe on breakout terminal **5** (ground).
- Red probe on terminal **2**, then terminal **3**.
- The one reading about **−5 to −6 V** is the **output**; the one near **0 V** is the **input**.

Write down: OUTPUT = terminal __, INPUT = terminal __.

*No multimeter:* assume output = 2 and input = 3. Step 7 confirms it safely.

## Step 5: find the four wires you need on the mini-DIN-8 cable

Only four of the cable's eight wires are used. Use the colour chart that came with the cable (pin
number → wire colour) to find them:

| Mac pin | Signal | Direction | Wire colour (from the chart) |
| --- | --- | --- | --- |
| 3 | TxD− (the Mac sends) | Mac output | ______ |
| 4 | Ground | — | ______ |
| 5 | RxD− (the Mac receives) | Mac input | ______ |
| 8 | RxD+ | Mac input | ______ |

Pins 1, 2, 6 and 7 (and any bare shield wire) aren't used. Wrap the end of each one **separately** in
tape so none of them can touch anything.

If you can't find the chart, send me what it says or a description, and don't guess.

## Step 6: check the wires against the Plus (multimeter; do this once the Plus floppy is ready)

This confirms the colour chart before anything is connected. Plug the mini-DIN-8 into the Plus's
**modem port** (the one with the telephone icon) with **nothing** on the other end. Keep the bare
wires apart from each other.

Start the Plus with the modem floppy and open the app's **Modem Terminal**, so the port is switched on.
Then, with the black probe on the **pin 4** wire, measure the others:

- **Pin 3** should read clearly negative, about −4 to −6 V.
- **Pin 6** (unused) should read about the same amount but positive.
- **Pins 5 and 8** should read near 0 V, because they're inputs.

If pin 3 isn't clearly negative, or pin 5 or 8 shows several volts, the chart and the cable disagree.
**Stop and send me your readings.**

## Step 7: connect the Mac's receive line first (safe even if a guess was wrong)

The Mac's pin 5 is an input, so connecting it to either DB9 pin can't harm anything.

1. Screw the **pin 4** wire and the **pin 8** wire into breakout terminal **5**, both in the same
   terminal.
2. Screw the **pin 5** wire into the module's **output** terminal (from Step 4; usually 2).
3. Leave the **pin 3** wire taped and unconnected for now.
4. Power the ESP32 from the charger. For its first minute it sends `PLUS MODEM READY` on the line every
   2 seconds, until the Mac sends anything.
5. On the Plus, open **Modem Terminal**. You should see `PLUS MODEM READY` lines.
   - If nothing appears, move the pin 5 wire to the other terminal (3), restart the ESP32 and look
     again.

## Step 8: connect the Mac's send line

Screw the **pin 3** wire into the remaining terminal: the module's **input**, usually 3. In Modem
Terminal, type `AT` and press Return. The modem answers **OK**.

The hardware is done. Put the breakout's plastic shell on and keep the ESP32 and module somewhere they
can't short against metal, for example in a small box or on the desk away from the Plus's vents.

## Step 9: Wi-Fi and first use

- **Wi-Fi** (2.4 GHz only): from the MacBook with the ESP32 on USB, run `python3 modem/tools/setup.py wifi`
  and type the network name and password at the prompts. It remembers several networks; for eduroam,
  run `python3 modem/tools/setup.py eduroam` (your eduroam username and password). You can also do it from the Plus
  in Modem Terminal; `AT+HELP` lists the commands.
- **Chat:** Ask AI on the Plus, with the dial string set to `ai`. The ESP32 then calls the model itself,
  so no other computer is needed and it works on campus. API keys are stored on the ESP32 from the
  MacBook (`python3 modem/tools/setup.py key anthropic`, typed at a hidden prompt), never on the Plus.
- **Files:** the Shuttle app on the Plus, with the server running on the MacBook. That works on your home
  network; campus Wi-Fi blocks devices from reaching each other.

The firmware, the tools and the Plus floppy are being built now; I'll tell you when each step's
software is ready.
