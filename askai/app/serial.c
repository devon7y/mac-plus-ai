/*
 * Ask AI: the Serial Driver (.AIn/.AOut on the modem port, .BIn/.BOut on the printer port), used
 * through parameter blocks so no glue is needed:
 *   - Control 8 (SerReset), 9 (SerSetBuf: a 16 KB input buffer), 10/14 (handshake), 17/18 (DTR);
 *   - Status 2 (SerGetBuf: bytes waiting), 8 (SerStatus: errors);
 *   - one asynchronous read always pending (PBReadAsync), so bytes are collected while the app draws;
 *   - asynchronous writes from a small queue (PBWriteAsync).
 * Works under System 6 (Plus ROM drivers) and System 7.
 */
#include <Devices.h>
#include <Files.h>
#include <MacMemory.h>
#include <OSUtils.h>
#include <string.h>
#include "serial.h"

/* SerReset's baud field is the SCC time constant: rate = 3.672 MHz / 32 / (TC + 2) on a real Plus or SE.
   MAME 0.289 clocks the Plus's SCC baud generator from 15.6672 MHz / 4 = 3.9168 MHz instead
   (mac128.cpp, C3_7M), so in MAME every rate is 6.7% fast and no standard rate meets its null modem.
   The hidden test speed (prefs only) uses TC 15: 6,750 baud on a real Mac, exactly 7,200 in MAME. */
const long serBauds[NBAUDS + 1] = {1200, 2400, 9600, 19200, 38400, 57600, 7200};
static const short baudCodes[NBAUDS + 1] = {94, 46, 10, 4, 1, 0, 15};   /* baud1200 ... baud57600, test */
#define CFG_8N1 (3072 + 0 + 16384)                                  /* data8 + noParity + stop10 */
#define INBUF 16384

static short inRef, outRef, isOpen;
static Ptr inBuf;
static long errCount;

/* reads: one PBReadAsync pending at a time; completed bytes are kept in rq until ser_read takes them */
static ParamBlockRec rdPB;
static short rdPending;
static char rdBuf[512];
static char rq[2048];
static long rqLen;

/* writes */
static ParamBlockRec wrPB;
static short wrPending;
static char wrBuf[1024];
static char wq[4096];
static long wqLen;

static OSErr ctl(short ref, short code, const void *p, short n)
{
    CntrlParam pb;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = ref;
    pb.csCode = code;
    if (p && n) BlockMove(p, pb.csParam, n);
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr stat(short ref, short code, void *p, short n)
{
    CntrlParam pb;
    OSErr e;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = ref;
    pb.csCode = code;
    e = PBStatusSync((ParmBlkPtr)&pb);
    if (p && n) BlockMove(pb.csParam, p, n);
    return e;
}

static OSErr openDrv(const unsigned char *name, short *ref)
{
    ParamBlockRec pb;
    OSErr e;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioPermssn = 0;          /* fsCurPerm */
    e = PBOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return e;
}

static void closeDrv(short ref)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    PBKillIOSync(&pb);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    PBCloseSync(&pb);
}

OSErr ser_open(short port, short baudIdx, short flow)
{
    OSErr e;
    short cfg;
    unsigned char shk[8];
    struct { Ptr p; short len; } sb;
    if (isOpen) ser_close();
    if (baudIdx < 0 || baudIdx > BAUD_MAMETEST) baudIdx = 3;
    e = openDrv(port ? "\p.BOut" : "\p.AOut", &outRef);    /* output first, then input */
    if (e != noErr) return e;
    e = openDrv(port ? "\p.BIn" : "\p.AIn", &inRef);
    if (e != noErr) { closeDrv(outRef); return e; }
    cfg = (short)(baudCodes[baudIdx] + CFG_8N1);
    ctl(outRef, 8, &cfg, 2);
    ctl(inRef, 8, &cfg, 2);
    if (!inBuf) inBuf = NewPtr(INBUF);
    if (inBuf) {
        sb.p = inBuf;
        sb.len = INBUF;
        ctl(inRef, 9, &sb, 6);
    }
    /* SerShk: fXOn fCTS xOn xOff errs evts fInX fDTR(csCode 14 only) */
    memset(shk, 0, sizeof shk);
    shk[2] = 0x11;
    shk[3] = 0x13;
    if (flow == FLOW_XON) { shk[0] = 1; shk[6] = 1; }
    if (flow == FLOW_HW) {
        shk[1] = 1;
        shk[7] = 1;
        if (ctl(outRef, 14, shk, 8) != noErr) ctl(outRef, 10, shk, 8);
    } else ctl(outRef, 10, shk, 8);
    ctl(outRef, 17, NULL, 0);            /* assert DTR (HSKo) */
    isOpen = 1;
    rdPending = wrPending = 0;
    rqLen = wqLen = 0;
    errCount = 0;
    ser_poll();
    return noErr;
}

void ser_close(void)
{
    struct { Ptr p; short len; } sb;
    ParamBlockRec pb;
    if (!isOpen) return;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = inRef;
    PBKillIOSync(&pb);                   /* stop the pending read */
    rdPending = 0;
    sb.p = NULL;                         /* back to the driver's own buffer: ours lives in the app heap */
    sb.len = 0;
    ctl(inRef, 9, &sb, 6);
    closeDrv(inRef);
    closeDrv(outRef);
    wrPending = 0;
    isOpen = 0;
    rqLen = wqLen = 0;
}

int ser_isOpen(void) { return isOpen; }

void ser_setDTR(int on)
{
    if (isOpen) ctl(outRef, on ? 17 : 18, NULL, 0);
}

long ser_errors(void) { return errCount; }

static void startRead(void)
{
    long n = 0;
    stat(inRef, 2, &n, 4);               /* SerGetBuf: bytes waiting */
    if (n < 1) n = 1;                    /* nothing yet: a 1-byte read waits for the next byte */
    if (n > (long)sizeof rdBuf) n = sizeof rdBuf;
    if (n > (long)sizeof rq - rqLen) n = (long)sizeof rq - rqLen;
    if (n < 1) return;                   /* our queue is full: read again after ser_read */
    memset(&rdPB, 0, sizeof rdPB);
    rdPB.ioParam.ioRefNum = inRef;
    rdPB.ioParam.ioBuffer = rdBuf;
    rdPB.ioParam.ioReqCount = n;
    rdPending = 1;
    PBReadAsync(&rdPB);
}

static void startWrite(void)
{
    long n = wqLen > (long)sizeof wrBuf ? (long)sizeof wrBuf : wqLen;
    if (n <= 0) return;
    BlockMove(wq, wrBuf, n);
    BlockMove(wq + n, wq, wqLen - n);
    wqLen -= n;
    memset(&wrPB, 0, sizeof wrPB);
    wrPB.ioParam.ioRefNum = outRef;
    wrPB.ioParam.ioBuffer = wrBuf;
    wrPB.ioParam.ioReqCount = n;
    wrPending = 1;
    PBWriteAsync(&wrPB);
}

void ser_poll(void)
{
    short k;
    if (!isOpen) return;
    for (k = 0; k < 8; k++) {            /* reads that completed (possibly at once) */
        if (rdPending) {
            volatile OSErr *res = &rdPB.ioParam.ioResult;
            if (*res > 0) break;         /* still waiting for a byte */
            rdPending = 0;
            if (*res == noErr || rdPB.ioParam.ioActCount > 0) {
                long got = rdPB.ioParam.ioActCount;
                if (got > (long)sizeof rq - rqLen) got = (long)sizeof rq - rqLen;
                BlockMove(rdBuf, rq + rqLen, got);
                rqLen += got;
            }
            if (*res != noErr) {         /* overrun, parity, framing: counted, then carry on */
                unsigned char st[8];
                errCount++;
                stat(inRef, 8, st, 8);   /* SerStatus clears the cumulative errors */
            }
        }
        if (!rdPending) startRead();
        if (!rdPending) break;           /* our queue is full */
    }
    if (wrPending) {
        volatile OSErr *res = &wrPB.ioParam.ioResult;
        if (*res <= 0) wrPending = 0;
    }
    if (!wrPending && wqLen) startWrite();
}

long ser_read(char *dst, long max)
{
    long n;
    ser_poll();
    n = rqLen < max ? rqLen : max;
    if (n <= 0) return 0;
    BlockMove(rq, dst, n);
    BlockMove(rq + n, rq, rqLen - n);
    rqLen -= n;
    return n;
}

void ser_write(const char *s, long n)
{
    if (!isOpen) return;
    if (n > (long)sizeof wq - wqLen) n = (long)sizeof wq - wqLen;   /* overflow: drop (never happens: lines are short) */
    BlockMove(s, wq + wqLen, n);
    wqLen += n;
    ser_poll();
}

int ser_writeIdle(void)
{
    ser_poll();
    return !wrPending && !wqLen;
}
