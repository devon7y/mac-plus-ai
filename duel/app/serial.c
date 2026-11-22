/* Orbit Duel: the serial cable transport (modem port, .AOut / .AIn).
 *
 * 8 data bits, no parity, 1 stop bit, no handshaking (a 3-wire null-modem link is enough).
 * Speed: SerReset time constant 4 = 19,200 baud (19,125 on the real 3.672 MHz SCC clock).
 * Test speed: MAME 0.289 clocks the Plus/SE SCC at 3.9168 MHz instead of 3.672 MHz, so no standard
 * rate matches its null-modem device; time constant 15 gives exactly 7,200 baud there (6,750 on a
 * real Mac), which the test scripts select (askai/HANDOFF.md found this first).
 *
 * Frames: 0x7E, length, payload, CRC-16 (CCITT, high byte first). The receiver hunts for 0x7E and
 * throws away anything whose CRC is wrong. Writes are asynchronous (one in flight; a packet that
 * finds the line busy is dropped, and the lockstep layer resends its contents anyway).
 */
#include <Multiverse.h>      /* (not <Serial.h>: on a case-insensitive disk it would find serial.c's header) */
#include "serlink.h"

static short in_ref, out_ref, is_open;
static ParamBlockRec wpb;
static unsigned char txbuf[SER_MAX + 4];
static unsigned char *inbuf;                    /* the driver's input buffer (replaces its 64 bytes) */
#define INBUF 2048
static unsigned char rx[2 * (SER_MAX + 4)];     /* bytes read, not yet parsed */
static short rxn;
long ser_bad;                                   /* frames thrown away (CRC or length) */

static unsigned short crc16(const unsigned char *p, short n)
{
    unsigned short c = 0xFFFF;
    short i;
    while (n-- > 0) {
        c ^= (unsigned short)*p++ << 8;
        for (i = 0; i < 8; i++)
            c = (c & 0x8000) ? (unsigned short)((c << 1) ^ 0x1021) : (unsigned short)(c << 1);
    }
    return c;
}

short ser_open(short test_speed)
{
    OSErr e;
    SerShk shk;
    short i;
    if (is_open)
        return 0;
    e = OpenDriver("\p.AOut", &out_ref);
    if (e)
        return e;
    e = OpenDriver("\p.AIn", &in_ref);
    if (e)
        return e;
    /* 8N1: stop10 (0x4000) + data8 (0x0C00) + no parity + the baud time constant */
    e = SerReset(out_ref, (short)(0x4000 | 0x0C00 | (test_speed ? 15 : 4)));
    if (e)
        return e;
    for (i = 0; i < (short)sizeof shk; i++)
        ((unsigned char *)&shk)[i] = 0;          /* no XON/XOFF, no CTS handshake */
    SerHShake(out_ref, &shk);
    SerHShake(in_ref, &shk);
    if (!inbuf)
        inbuf = (unsigned char *)NewPtr(INBUF);
    if (inbuf)
        SerSetBuf(in_ref, (Ptr)inbuf, INBUF);
    wpb.ioParam.ioResult = 0;
    rxn = 0;
    is_open = 1;
    return 0;
}

void ser_close(void)
{
    ParamBlockRec k;
    short i;
    if (!is_open)
        return;
    for (i = 0; i < (short)sizeof k; i++)
        ((unsigned char *)&k)[i] = 0;
    k.cntrlParam.ioCRefNum = out_ref;
    PBKillIOSync(&k);                           /* stop a write still in progress */
    SerSetBuf(in_ref, (Ptr)inbuf, 0);           /* back to the driver's own buffer */
    CloseDriver(in_ref);
    CloseDriver(out_ref);
    is_open = 0;
}

short ser_is_open(void) { return is_open; }

short ser_send(const unsigned char *p, short n)
{
    unsigned short c;
    short i;
    if (!is_open || n > SER_MAX)
        return -1;
    if (wpb.ioParam.ioResult > 0)
        return -1;                              /* still sending the previous frame */
    txbuf[0] = 0x7E;
    txbuf[1] = (unsigned char)n;
    for (i = 0; i < n; i++)
        txbuf[2 + i] = p[i];
    c = crc16(txbuf + 1, n + 1);
    txbuf[2 + n] = (unsigned char)(c >> 8);
    txbuf[3 + n] = (unsigned char)c;
    wpb.ioParam.ioCompletion = 0;
    wpb.ioParam.ioRefNum = out_ref;
    wpb.ioParam.ioBuffer = (Ptr)txbuf;
    wpb.ioParam.ioReqCount = n + 4;
    wpb.ioParam.ioPosMode = 0;
    wpb.ioParam.ioPosOffset = 0;
    PBWriteAsync(&wpb);
    return 0;
}

/* the next good frame's payload, or 0 */
short ser_recv(unsigned char *p, short max)
{
    long avail = 0;
    short i, len;
    if (!is_open)
        return 0;
    SerGetBuf(in_ref, &avail);
    if (avail > 0) {
        long room = (long)sizeof rx - rxn;
        if (avail > room)
            avail = room;
        if (avail > 0 && FSRead(in_ref, &avail, rx + rxn) == noErr)
            rxn += (short)avail;
    }
    for (;;) {
        for (i = 0; i < rxn && rx[i] != 0x7E; i++)
            ;
        if (i) {                                /* drop bytes before the frame start */
            short k;
            for (k = i; k < rxn; k++)
                rx[k - i] = rx[k];
            rxn -= i;
        }
        if (rxn < 2)
            return 0;
        len = rx[1];
        if (len == 0 || len > SER_MAX) {
            ser_bad++;
            rx[0] = 0;                          /* not a frame start after all: hunt again */
            continue;
        }
        if (rxn < len + 4)
            return 0;                           /* wait for the rest */
        if (crc16(rx + 1, len + 1) == (unsigned short)((rx[2 + len] << 8) | rx[3 + len]) && len <= max) {
            short k;
            for (k = 0; k < len; k++)
                p[k] = rx[2 + k];
            for (k = len + 4; k < rxn; k++)
                rx[k - len - 4] = rx[k];
            rxn -= len + 4;
            return len;
        }
        ser_bad++;
        rx[0] = 0;
    }
}
