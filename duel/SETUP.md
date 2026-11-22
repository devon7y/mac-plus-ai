# Orbit Duel: setting up a game between two Macs

Orbit Duel is a duel between two ships around a star, in the spirit of Spacewar! (1962). The first to 5 wins.
You can play the computer or a friend on the same keyboard, or link two Macs (the Plus and the SE) and play one
on each.

## Disks

- **Mac Plus:** the 800K floppy `orbitduel.img`. It starts straight into the game and runs in 1 MB. It also
  starts an SE.
- **Mac SE:** the 1.44 MB floppy `orbitduel_se.img` (System 6.0.7, with the Chooser in the Apple menu), or the
  800K floppy. Orbit Duel also runs under System 7: copy the "Orbit Duel" application to the SE's hard disk.

## Option 1: serial cable (simplest)

**You need one cable:** a Mac-to-Mac **null-modem** cable with a mini-DIN-8 plug at each end. Apple's ImageWriter II
printer cable ("System Peripheral-8" cable, 590-0552) is wired this way, so it works. A plain modem cable does
not, because it doesn't cross the send and receive wires.

1. Plug the cable into the **modem port** (phone icon) of both Macs.
2. On both Macs, choose **4, Serial cable game** on the title screen, or **Game > Serial Cable Game**.
3. The Macs find each other within a second or two and the game starts. The Mac that is picked to host flies the
   Needle (left); the other flies the Wedge (right).

The link runs at 19,200 baud with no handshaking, so only the send, receive and ground wires matter.

## Option 2: AppleTalk (LocalTalk, printer ports)

**You need either of these:**

- **Two LocalTalk or PhoneNet connector boxes**, one per Mac, joined by their cable. PhoneNet boxes use an
  ordinary phone cord; put a terminator in the empty socket of each box if the boxes don't terminate themselves.
- **For two Macs only, and usually fine for a short run:** the same ImageWriter II cable plugged straight into
  the **printer ports** of both Macs.

**Steps:**

1. Unplug any printer from the printer ports.
2. On both Macs, choose **3, Network game over AppleTalk**.
   - If AppleTalk is off, Orbit Duel says so and offers to turn it on (**Return**). This is the same setting as
     the Chooser's "AppleTalk Active" button, and the Mac remembers it.
   - Only do this when the printer port goes to the other Mac.
   - On the SE you can use **Apple menu > Chooser > AppleTalk Active** instead. Under System 7 you may have to
     restart after that.
3. Each Mac shows its name on the network, for example "Mac Plus #12" (the Chooser's Macintosh name if it has
   one) and a list of the other Macs running Orbit Duel.
4. On one Mac, pick the other from the list with **W / S** or the mouse, then press **Return**. The other Mac
   accepts by itself and the game starts.
5. The Mac that invited flies the Needle.

## Playing

| Action | Keys |
| --- | --- |
| Turn | A / D (or the arrow keys) |
| Thrust | W |
| Fire | Space or Shift |
| Hyperspace (risky: each jump in a round is more likely to go wrong) | S |
| Pause (pauses both Macs) | Return |
| Sound on/off | M |
| Leave the game (the other Mac is told) | Esc (on the Plus: the ` key) |

The Plus keyboard reports only two letter keys at once, so press **Shift** to fire while you turn and thrust.

After a match, Space or Return on either Mac starts a rematch.

## If something goes wrong

- **"Waiting for the other Mac..."** means nothing has arrived for 3 s. Check the cable and that both Macs use
  the same ports (modem with modem, or printer with printer). After 30 s with nothing, the game stops.
- **"OUT OF SYNC"** means the two Macs disagree about the game. That shouldn't happen. Please note what was
  going on.
- **"The printer port is in use by another driver"**: a serial printer driver has the port. Restart the Mac
  without printing and try again.
