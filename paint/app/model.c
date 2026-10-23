/*
 * The drawing model: load "Doodle Model" (paint/bench/model_file.py format) into one block,
 * expanding the packed weight codes into the kernels' RAM formats, and drive the kernels
 * (gen_kernels.py) one step at a time. See paint/bench/FORMAT.md.
 */
#include "doodle.h"
#include "model.h"
#include "kernels.h"

#define NOUT 123
#define NEMB 32

static Ptr gBlock;
static Boolean gLoaded;
static short gH, gNCat;
static unsigned char *gNames;          /* NCAT Pascal strings */
static unsigned char **gNameIdx;
long gModelBytes;

/* buffered reader */
static short rRef;
static unsigned char rBuf[2048];
static short rPos, rLen;
static OSErr rErr;

static void RdBytes(void *dst, long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n > 0 && !rErr) {
        if (rPos >= rLen) {
            long cnt = sizeof rBuf;
            rErr = FSRead(rRef, &cnt, rBuf);
            if (rErr == eofErr && cnt > 0)
                rErr = noErr;
            rLen = (short)cnt;
            rPos = 0;
            if (cnt == 0 && !rErr)
                rErr = eofErr;
            continue;
        }
        {
            short k = rLen - rPos;
            if (k > n)
                k = (short)n;
            BlockMove(rBuf + rPos, d, k);
            rPos += k;
            d += k;
            n -= k;
        }
    }
}

/* Expand rows of packed codes (one byte per ternary group or 4-bit pair) into kernel codes. */
static void ReadCodes(short *dst, short rows, short H, short fileKind, short ramKind)
{
    unsigned char row[192];
    short r, g, ng = fileKind == 0 ? H / 4 : H / 2;
    for (r = 0; r < rows && !rErr; r++) {
        RdBytes(row, ng);
        if (ramKind == 0) {                       /* W4: 16-bit (g*81 + idx)*2 */
            for (g = 0; g < ng; g++)
                *dst++ = (short)((g * 81 + row[g]) * 2);
        } else if (ramKind == 1) {                /* B4: byte idx*2 */
            unsigned char *b = (unsigned char *)dst;
            for (g = 0; g < ng; g++)
                b[(long)r * ng + g] = (unsigned char)(row[g] * 2);
        } else {                                   /* P4: pairs >= 128 first, own window */
            short first = ng > 128 ? 128 : 0, p, q;
            for (q = 0; q < ng - 128 && ng > 128; q++)
                *dst++ = (short)((q * 256 + row[first + q]) * 2 - 32768);
            for (p = 0; p < (ng > 128 ? 128 : ng); p++)
                *dst++ = (short)((p * 256 + row[p]) * 2 - 32768);
        }
    }
}

static long CodeBytes(short kind, short rows, short H)
{
    return kind == 1 ? (long)rows * (H / 4) : (long)rows * (kind == 0 ? H / 4 : H / 2) * 2;
}

static long LutBytes(short kind, short H)
{
    return kind == 2 ? (long)(H / 2) * 512 : (long)(H / 4) * 162;
}

/* Open "Doodle Model" read-only: in the application's own folder (found from the app's open
   resource file), else the default folder, else the top level of the app's volume. */
static OSErr HOpenRead(ConstStr255Param name, short vref, long dir, short *ref)
{
    HParamBlockRec pb;
    OSErr err;
    short i;
    for (i = 0; i < (short)sizeof pb; i++)
        ((char *)&pb)[i] = 0;
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vref;
    pb.ioParam.ioPermssn = fsRdPerm;
    pb.fileParam.ioDirID = dir;
    err = PBHOpenSync(&pb);
    *ref = pb.ioParam.ioRefNum;
    return err;
}

static OSErr OpenModel(short *ref)
{
    static const unsigned char name[] = "\pDoodle Model";
    FCBPBRec fcb;
    short i, appRef;
    __asm__ volatile("move.w 0x900.w,%0" : "=d"(appRef));     /* CurApRefNum */
    for (i = 0; i < (short)sizeof fcb; i++)
        ((char *)&fcb)[i] = 0;
    fcb.ioRefNum = appRef;
    if (PBGetFCBInfoSync(&fcb) == noErr) {
        if (HOpenRead(name, fcb.ioFCBVRefNum, fcb.ioFCBParID, ref) == noErr) {
            dbg.modelWhere = 1;
            return noErr;
        }
    }
    if (FSOpen(name, 0, ref) == noErr) {
        dbg.modelWhere = 2;
        return noErr;
    }
    if (fcb.ioFCBVRefNum && HOpenRead(name, fcb.ioFCBVRefNum, 2, ref) == noErr) {
        dbg.modelWhere = 3;
        return noErr;
    }
    return fnfErr;
}

Boolean ModelLoaded(void)
{
    return gLoaded;
}

short ModelNCat(void)
{
    return gNCat;
}

const unsigned char *ModelName(short i)
{
    static const unsigned char unknown[] = "\p?";
    return (i >= 0 && i < gNCat) ? gNameIdx[i] : unknown;
}

OSErr ModelLoad(void)
{
    unsigned char hdr[64];
    short H, kwh, kwo, ramK, ramKO, i, R;
    Boolean r2, mixed;
    long namesOff, dataOff, size, off;
    long sizes[40];
    Ptr p;
    if (gLoaded)
        return noErr;
    rErr = OpenModel(&rRef);
    if (rErr)
        return rErr;
    rPos = rLen = 0;
    RdBytes(hdr, 64);
    if (rErr || hdr[0] != 'D' || hdr[1] != 'M' || hdr[2] != 'D' || hdr[3] != '1') {
        FSClose(rRef);
        return rErr ? rErr : -1;
    }
#define HW(o) (*(short *)(hdr + (o)))
    H = HW(8);
    kwh = HW(10);
    kwo = HW(12);
    gNCat = HW(30);
    r2 = (HW(46) & 1) != 0;
    namesOff = *(long *)(hdr + 36);
    dataOff = *(long *)(hdr + 40);
    if (HW(14) != 0 || H % 8 || H > 384 || H < 32 || (kwh != 0 && kwh != 2) || (kwo != 0 && kwo != 2)
        || HW(32) != NEMB || HW(34) != NOUT || gNCat < 1 || gNCat > 400) {
        FSClose(rRef);
        return -2;
    }
    R = 3 * H;
    /* names */
    gNames = (unsigned char *)NewPtr(dataOff - namesOff);
    gNameIdx = (unsigned char **)NewPtr(4L * gNCat);
    if (!gNames || !gNameIdx) {
        FSClose(rRef);
        return memFullErr;
    }
    RdBytes(gNames, dataOff - namesOff);
    for (i = 0, off = 0; i < gNCat; i++) {
        gNameIdx[i] = gNames + off;
        off += 1 + gNames[off];
    }
    /* ternary runs as W4 (16-bit codes) when it fits, else as B4 (byte codes) */
    for (ramK = kwh == 0 ? 0 : 2;; ramK = 1) {
        ramKO = kwo == 0 ? (ramK == 1 ? 1 : 0) : 2;
        mixed = ramK != ramKO || HW(16) != HW(18);
        i = 0;
        sizes[i++] = CodeBytes(ramK, R, H);            /* 0 U */
        sizes[i++] = CodeBytes(ramKO, NOUT, H);        /* 1 WY */
        sizes[i++] = LutBytes(ramK, H);                /* 2 LUT */
        sizes[i++] = mixed ? LutBytes(ramKO, H) : 0;   /* 3 LUTO (separately quantized output only) */
        sizes[i++] = 4L * H;                           /* 4 M */
        sizes[i++] = 4L * H;                           /* 5 MN */
        sizes[i++] = 2L * NOUT;                        /* 6 MY */
        sizes[i++] = 2L * NOUT;                        /* 7 BY */
        sizes[i++] = 8L * H;                           /* 8 B0 */
        sizes[i++] = 8L * H;                           /* 9 MC */
        sizes[i++] = 4L * H * NEMB;                    /* 10 WC */
        sizes[i++] = 2L * gNCat * NEMB;                /* 11 EMB */
        sizes[i++] = 6L * R;                           /* 12 P */
        sizes[i++] = 2L * R;                           /* 13 WDX */
        sizes[i++] = 2L * R;                           /* 14 WDY */
        sizes[i++] = 8L * H;                           /* 15 E (fold: 3H gate rows + H) */
        sizes[i++] = 4L * R;                           /* 16 CX */
        sizes[i++] = 2L * R;                           /* 17 S */
        sizes[i++] = 2L * R;                           /* 18 GV */
        sizes[i++] = 2L * H;                           /* 19 HST */
        sizes[i++] = 2L * H;                           /* 20 HQ */
        sizes[i++] = mixed ? 2L * H : 0;               /* 21 HQO */
        sizes[i++] = 2L * NOUT;                        /* 22 SY */
        sizes[i++] = 2L * NOUT;                        /* 23 O */
        sizes[i++] = 80;                               /* 24 TL */
        sizes[i++] = 32;                               /* 25 SAMP */
        sizes[i++] = r2 ? CodeBytes(ramKO, NOUT, H) : 0;   /* 26 WY2 (two-term output) */
        sizes[i++] = r2 ? 2L * NOUT : 0;               /* 27 MY2 */
        sizes[i++] = r2 ? 2L * NOUT : 0;               /* 28 SY2 */
        for (size = 0, i = 0; i < 29; i++)
            size += (sizes[i] + 3) & ~3L;
        gBlock = NewPtr(size);
        if (gBlock || ramK != 0)
            break;
    }
    if (!gBlock) {
        FSClose(rRef);
        return memFullErr;
    }
    gModelBytes = size;
    {
        Ptr b[29];
        for (p = gBlock, i = 0; i < 29; i++) {
            b[i] = p;
            p += (sizes[i] + 3) & ~3L;
        }
        DKP(U) = b[0];
        DKP(WY) = b[1];
        DKP(LUT) = b[2];
        DKP(LUTO) = b[3];
        DKP(M) = b[4];
        DKP(MN) = b[5];
        DKP(MY) = b[6];
        DKP(BY) = b[7];
        DKP(B0) = b[8];
        DKP(MC) = b[9];
        DKP(WC) = b[10];
        DKP(EMB) = b[11];
        DKP(P) = b[12];
        DKP(WDX) = b[13];
        DKP(WDY) = b[14];
        DKP(E) = b[15];
        DKP(CX) = b[16];
        DKP(S) = b[17];
        DKP(GV) = b[18];
        DKP(HST) = b[19];
        DKP(HQ) = b[20];
        DKP(HQO) = b[21];
        DKP(SY) = b[22];
        DKP(O) = b[23];
        DKP(TL) = b[24];
        DKP(SAMP) = b[25];
        DKP(WY2) = b[26];
        DKP(MY2) = b[27];
        DKP(SY2) = b[28];
        DKP(A2) = ramK == 2 ? b[2] + 32768 : b[2];
        DKP(A3) = ramK == 2 ? b[2] + 65536L + 32768 : b[2];
        DKP(A2O) = ramKO == 2 ? b[3] + 32768 : b[3];
        DKP(A3O) = ramKO == 2 ? b[3] + 65536L + 32768 : b[3];
        DKP(S_2) = b[17] + 4L * H;
        DKP(CX_2) = b[16] + 8L * H;
        DKP(GV_1) = b[18] + 2L * H;
        DKP(GV_2) = b[18] + 4L * H;
        /* data, in file order */
        ReadCodes((short *)b[0], R, H, kwh, ramK);
        ReadCodes((short *)b[1], NOUT, H, kwo, ramKO);
        RdBytes(b[4], sizes[4]);
        RdBytes(b[5], sizes[5]);
        RdBytes(b[6], sizes[6]);
        RdBytes(b[7], sizes[7]);
        RdBytes(b[8], sizes[8]);
        RdBytes(b[9], sizes[9]);
        RdBytes(b[10], sizes[10]);
        RdBytes(b[11], sizes[11]);
        RdBytes(b[12], sizes[12]);
        RdBytes(b[13], sizes[13]);
        RdBytes(b[14], sizes[14]);
        if (r2) {
            ReadCodes((short *)b[26], NOUT, H, kwo, ramKO);
            RdBytes(b[27], sizes[27]);
        }
    }
    FSClose(rRef);
    if (rErr) {
        DisposePtr(gBlock);
        gBlock = NULL;
        return rErr;
    }
    gH = H;
    DKW(H) = H;
    DKW(R) = R;
    DKW(G) = ramK == 2 ? H / 2 : H / 4;
    DKW(GO) = ramKO == 2 ? H / 2 : H / 4;
    DKW(KIND) = ramK;
    DKW(KINDO) = ramKO;
    DKW(A) = HW(16);
    DKW(AO) = HW(18);
    DKW(MIXED) = mixed;
    DKW(SHR) = HW(20);
    DKW(SHO) = HW(22);
    DKW(SHA) = HW(24);
    DKW(SHB) = HW(26);
    DKW(FQ) = HW(28);
    DKW(SHX) = HW(44) ? HW(44) : 6;
    DKW(R2O) = r2;
    DKW(SHO2) = HW(48);
    dk_init();
    gLoaded = true;
    dbg.fiModel = (unsigned long)H | ((unsigned long)ramK << 16) | ((unsigned long)ramKO << 20);
    return noErr;
#undef HW
}

void ModelBegin(short cat, unsigned long seed, short temp)
{
    /* Finish menu settings: mixture temperature (INVT = 256/t, HLT = 512 ln t) and pen temperature
       (INVTP = 256/t_pen); paint/bench/ref.py TEMP_SETTINGS holds the same values */
    static const short invt[3] = {853, 512, 320}, hlt[3] = {-616, -355, -114}, invtp[3] = {341, 341, 341};
    DKW(CAT) = cat;
    DKL(SEED) = (long)(seed ? seed : 0x2545F491UL);
    DKW(INVT) = invt[temp];
    DKW(HLT) = hlt[temp];
    DKW(INVTP) = invtp[temp];
    dk_fold();
}

void ModelPrime(short dx, short dy, short pen)
{
    DKW(DX) = dx;
    DKW(DY) = dy;
    DKW(PEN) = pen;
    dk_prime();
}

void ModelFull(short dx, short dy, short pen)
{
    DKW(DX) = dx;
    DKW(DY) = dy;
    DKW(PEN) = pen;
    dk_full();
}

void ModelNext(void)
{
    dk_full();
}

void ModelSample(short *dx, short *dy, short *pen)
{
    short *s = (short *)DKP(SAMP);
    *pen = s[1];
    *dx = s[2];
    *dy = s[3];
}
