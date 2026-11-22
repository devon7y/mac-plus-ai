/*
 * Shuttle: files on the Mac's disks.
 *
 * Download: dl_begin() creates the file (type and creator from the listing) and sets both forks' end
 * of file at once, so a full disk is found before the transfer starts and the forks are allocated in
 * one piece. dl_write() takes the MacBinary stream as it comes (header, data fork, padding, resource
 * fork, padding) and writes the forks. dl_finish() sets the type, creator, Finder flags (clearing
 * "on desk", "inited" and "changed", so the Finder places the file and reads its icon) and the dates.
 * Upload: ul_read() produces a MacBinary II stream of a file (both forks opened read-only, so even
 * the running application can be sent).
 */
#include <Files.h>
#include <MacMemory.h>
#include <OSUtils.h>
#include <string.h>
#include "files.h"
#include "link.h"

static unsigned short crcOf(const unsigned char *p, long n) { return crc16(p, n, 0); }

static long pad128(long n) { return (n + 127) & ~127L; }

static unsigned long get32(const unsigned char *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static OSErr openFork(const unsigned char *name, short vRef, int rsrc, int readOnly, short *ref)
{
    ParamBlockRec pb;
    OSErr e;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioVRefNum = vRef;
    pb.ioParam.ioPermssn = readOnly ? fsRdPerm : fsRdWrPerm;
    e = rsrc ? PBOpenRFSync(&pb) : PBOpenSync(&pb);
    *ref = e == noErr ? pb.ioParam.ioRefNum : 0;
    return e;
}

/* ------------------------------------------------------------------ download */
static unsigned char dName[64];
static short dVol, dData, dRsrc, dOpen, dPhase;
static unsigned char hdr[128];
static long hdrGot, dRemain, dLen, rLen, expD, expR, wroteD, wroteR;
unsigned short dlCrc[2];             /* CRC-16 of what was written to each fork, without the resource fork's
                                        bytes 16-127 (the system writes its own copy of the catalog info there
                                        when the fork is closed); dl_verify() reads the file back and compares */

/* CRC of bytes p[0..n) that sit at fork position pos, leaving out positions 16-127 if skip */
static unsigned short crcPart(unsigned short c, const unsigned char *p, long n, long pos, int skip)
{
    if (skip && pos < 128) {
        long a = pos < 16 ? 16 - pos : 0;                /* bytes before position 16 */
        long b = 128 - pos;                               /* bytes before position 128 */
        if (a > n) a = n;
        if (b > n) b = n;
        c = crc16(p, a, c);
        return crc16(p + b, n - b, c);
    }
    return crc16(p, n, c);
}

OSErr dl_begin(const unsigned char *name, short vRefNum, OSType type, OSType creator, long dlen, long rlen)
{
    OSErr e;
    long n;
    memcpy(dName, name, name[0] + 1);
    dVol = vRefNum;
    dData = dRsrc = 0;
    hdrGot = 0;
    dPhase = 0;
    expD = dlen;
    expR = rlen;
    wroteD = wroteR = 0;
    dlCrc[0] = dlCrc[1] = 0;
    e = FSDelete(dName, dVol);                       /* Standard File already asked about replacing it */
    if (e != noErr && e != fnfErr) return e;
    e = Create(dName, dVol, creator, type);
    if (e != noErr) return e;
    dOpen = 1;
    e = openFork(dName, dVol, 0, 0, &dData);
    if (e == noErr) e = openFork(dName, dVol, 1, 0, &dRsrc);
    if (e == noErr && dlen) { n = dlen; e = SetEOF(dData, n); }
    if (e == noErr && rlen) { n = rlen; e = SetEOF(dRsrc, n); }
    if (e == noErr) e = SetFPos(dData, fsFromStart, 0);
    if (e == noErr) e = SetFPos(dRsrc, fsFromStart, 0);
    if (e != noErr) dl_abort();
    return e;
}

static OSErr writeFork(short ref, const unsigned char *p, long n)
{
    long k = n;
    OSErr e = FSWrite(ref, &k, p);
    return e != noErr ? e : (k == n ? noErr : ioErr);
}

OSErr dl_write(const unsigned char *p, long n)
{
    OSErr e;
    while (n > 0) {
        long k;
        switch (dPhase) {
        case 0:                                      /* the header */
            k = 128 - hdrGot;
            if (k > n) k = n;
            memcpy(hdr + hdrGot, p, k);
            hdrGot += k;
            p += k;
            n -= k;
            if (hdrGot == 128) {
                if (hdr[0] || hdr[74] || hdr[82] || hdr[1] < 1 || hdr[1] > 63) return ERR_MACBINARY;
                dLen = (long)get32(hdr + 83);
                rLen = (long)get32(hdr + 87);
                if (dLen != expD || rLen != expR) return ERR_MACBINARY;
                dPhase = 1;
                dRemain = dLen;
            }
            continue;
        case 1:                                      /* the data fork */
        case 3:                                      /* the resource fork */
            k = dRemain < n ? dRemain : n;
            if (k > 0) {
                if (dPhase == 1) dlCrc[0] = crcPart(dlCrc[0], p, k, wroteD, 0);
                else dlCrc[1] = crcPart(dlCrc[1], p, k, wroteR, 1);
                e = writeFork(dPhase == 1 ? dData : dRsrc, p, k);
                if (e != noErr) return e;
                if (dPhase == 1) wroteD += k; else wroteR += k;
                p += k;
                n -= k;
                dRemain -= k;
            }
            if (dRemain == 0) {
                dRemain = dPhase == 1 ? pad128(dLen) - dLen : pad128(rLen) - rLen;
                dPhase++;
            }
            continue;
        case 2:                                      /* padding */
        case 4:
            k = dRemain < n ? dRemain : n;
            p += k;
            n -= k;
            dRemain -= k;
            if (dRemain == 0) {
                dPhase++;
                dRemain = rLen;
            }
            continue;
        default:                                     /* anything after the end: ignored */
            return noErr;
        }
    }
    return noErr;
}

OSErr dl_finish(void)
{
    ParamBlockRec pb;
    OSErr e;
    unsigned short flags;
    if (hdrGot < 128 || wroteD != dLen || wroteR != rLen) { dl_abort(); return ERR_MACBINARY; }
    if (dData) FSClose(dData);
    if (dRsrc) FSClose(dRsrc);           /* (the File Manager then writes the file's catalog info into the
                                            resource fork's bytes 48-127, reserved for the system: normal) */
    dData = dRsrc = 0;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = dName;
    pb.fileParam.ioVRefNum = dVol;
    e = PBGetFInfoSync(&pb);
    if (e == noErr) {
        flags = (unsigned short)(hdr[73] << 8 | (hdr[122] >= 129 ? hdr[101] : 0));
        flags &= (unsigned short)~(0x0001 | 0x0100 | 0x0200);     /* on desk, inited, changed */
        memcpy(&pb.fileParam.ioFlFndrInfo.fdType, hdr + 65, 4);
        memcpy(&pb.fileParam.ioFlFndrInfo.fdCreator, hdr + 69, 4);
        pb.fileParam.ioFlFndrInfo.fdFlags = flags;
        pb.fileParam.ioFlFndrInfo.fdLocation.h = 0;
        pb.fileParam.ioFlFndrInfo.fdLocation.v = 0;
        pb.fileParam.ioFlFndrInfo.fdFldr = 0;
        if (get32(hdr + 91)) pb.fileParam.ioFlCrDat = get32(hdr + 91);
        if (get32(hdr + 95)) pb.fileParam.ioFlMdDat = get32(hdr + 95);
        pb.fileParam.ioFDirIndex = 0;
        e = PBSetFInfoSync(&pb);
    }
    dOpen = 0;
    FlushVol(NULL, dVol);
    return e;
}

/* Read the saved file back (both forks, through the File Manager) and compare with what was written:
   catches a bad floppy. buf: scratch space of n bytes. */
OSErr dl_verify(unsigned char *buf, long n)
{
    short k;
    for (k = 0; k < 2; k++) {
        short ref;
        long pos = 0, len = k ? rLen : dLen;
        unsigned short c = 0;
        OSErr e = openFork(dName, dVol, k, 1, &ref);
        if (e != noErr) return e;
        while (pos < len) {
            long m = len - pos < n ? len - pos : n;
            e = FSRead(ref, &m, buf);
            if (e != noErr && e != eofErr) break;
            if (m <= 0) { e = eofErr; break; }
            c = crcPart(c, buf, m, pos, k);
            pos += m;
            e = noErr;
        }
        FSClose(ref);
        if (e != noErr) return e;
        if (c != dlCrc[k]) return ERR_VERIFY;
    }
    return noErr;
}

void dl_discard(void)                /* after dl_finish: delete the saved file */
{
    FSDelete(dName, dVol);
    FlushVol(NULL, dVol);
}

void dl_abort(void)
{
    if (dData) FSClose(dData);
    if (dRsrc) FSClose(dRsrc);
    dData = dRsrc = 0;
    if (dOpen) FSDelete(dName, dVol);
    dOpen = 0;
    FlushVol(NULL, dVol);
}

/* ------------------------------------------------------------------ upload */
static short uData, uRsrc, uPhase;
static long uRemain, uDlen, uRlen, uHdrPos;
static unsigned char uHdr[128];

OSErr ul_begin(const unsigned char *name, short vRefNum, long *total)
{
    ParamBlockRec pb;
    OSErr e;
    unsigned short fl;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vRefNum;
    e = PBGetFInfoSync(&pb);
    if (e != noErr) return e;
    uDlen = pb.fileParam.ioFlLgLen;
    uRlen = pb.fileParam.ioFlRLgLen;
    memset(uHdr, 0, sizeof uHdr);
    uHdr[1] = name[0] > 63 ? 63 : name[0];
    memcpy(uHdr + 2, name + 1, uHdr[1]);
    memcpy(uHdr + 65, &pb.fileParam.ioFlFndrInfo.fdType, 4);
    memcpy(uHdr + 69, &pb.fileParam.ioFlFndrInfo.fdCreator, 4);
    fl = pb.fileParam.ioFlFndrInfo.fdFlags;
    uHdr[73] = (unsigned char)(fl >> 8);
    uHdr[75] = (unsigned char)(pb.fileParam.ioFlFndrInfo.fdLocation.v >> 8);
    uHdr[76] = (unsigned char)pb.fileParam.ioFlFndrInfo.fdLocation.v;
    uHdr[77] = (unsigned char)(pb.fileParam.ioFlFndrInfo.fdLocation.h >> 8);
    uHdr[78] = (unsigned char)pb.fileParam.ioFlFndrInfo.fdLocation.h;
    uHdr[81] = (unsigned char)((pb.fileParam.ioFlAttrib & 0x01) ? 1 : 0);    /* locked */
    put32(uHdr + 83, (unsigned long)uDlen);
    put32(uHdr + 87, (unsigned long)uRlen);
    put32(uHdr + 91, pb.fileParam.ioFlCrDat);
    put32(uHdr + 95, pb.fileParam.ioFlMdDat);
    uHdr[101] = (unsigned char)fl;
    uHdr[122] = uHdr[123] = 129;
    {
        unsigned short c = crcOf(uHdr, 124);
        uHdr[124] = (unsigned char)(c >> 8);
        uHdr[125] = (unsigned char)c;
    }
    uData = uRsrc = 0;
    e = openFork(name, vRefNum, 0, 1, &uData);
    if (e == noErr && uRlen) e = openFork(name, vRefNum, 1, 1, &uRsrc);
    if (e != noErr) { ul_end(); return e; }
    uPhase = 0;
    uHdrPos = 0;
    *total = 128 + pad128(uDlen) + pad128(uRlen);
    return noErr;
}

OSErr ul_read(unsigned char *p, long n)
{
    while (n > 0) {
        long k;
        OSErr e;
        switch (uPhase) {
        case 0:
            k = 128 - uHdrPos;
            if (k > n) k = n;
            memcpy(p, uHdr + uHdrPos, k);
            uHdrPos += k;
            if (uHdrPos == 128) { uPhase = 1; uRemain = uDlen; }
            break;
        case 1:
        case 3:
            k = uRemain < n ? uRemain : n;
            if (k > 0) {
                long got = k;
                e = FSRead(uPhase == 1 ? uData : uRsrc, &got, p);
                if (e != noErr && e != eofErr) return e;
                if (got != k) return eofErr;
                uRemain -= k;
            }
            if (uRemain == 0) {
                uRemain = uPhase == 1 ? pad128(uDlen) - uDlen : pad128(uRlen) - uRlen;
                uPhase++;
            }
            break;
        case 2:
        case 4:
            k = uRemain < n ? uRemain : n;
            memset(p, 0, k);
            uRemain -= k;
            if (uRemain == 0) { uPhase++; uRemain = uRlen; }
            break;
        default:
            k = n;
            memset(p, 0, k);
            break;
        }
        p += k;
        n -= k;
    }
    return noErr;
}

void ul_end(void)
{
    if (uData) FSClose(uData);
    if (uRsrc) FSClose(uRsrc);
    uData = uRsrc = 0;
}

/* ------------------------------------------------------------------ volumes */
long vol_free(short vRefNum, long *alBlk)
{
    HParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioVRefNum = vRefNum;
    pb.volumeParam.ioVolIndex = 0;
    if (PBHGetVInfoSync(&pb) != noErr) { *alBlk = 1024; return -1; }
    *alBlk = pb.volumeParam.ioVAlBlkSiz;
    return (long)pb.volumeParam.ioVFrBlk * pb.volumeParam.ioVAlBlkSiz;
}

long file_size(const unsigned char *name, short vRefNum)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    pb.fileParam.ioVRefNum = vRefNum;
    if (PBGetFInfoSync(&pb) != noErr) return -1;
    return pb.fileParam.ioFlPyLen + pb.fileParam.ioFlRPyLen;
}

short startup_vol(void)
{
    SysEnvRec env;
    HParamBlockRec pb;
    if (SysEnvirons(1, &env) != noErr) return 0;
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioVRefNum = env.sysVRefNum;
    pb.volumeParam.ioVolIndex = 0;
    if (PBHGetVInfoSync(&pb) != noErr) return env.sysVRefNum;
    return pb.volumeParam.ioVRefNum;
}

const char *files_errText(OSErr e)
{
    switch (e) {
    case dskFulErr: return "The disk is full.";
    case wPrErr: case vLckdErr: return "The disk is locked.";
    case fLckdErr: return "That file is locked.";
    case fBsyErr: case opWrErr: return "That file is in use (an open application or document).";
    case dirFulErr: return "The disk's directory is full.";
    case ioErr: return "The disk couldn't be read or written (I/O error).";
    case ERR_MACBINARY: return "The file arrived damaged (its MacBinary header didn't match). Try again.";
    case ERR_VERIFY: return "The saved file didn't read back correctly, so the disk may be bad. Shuttle "
                            "deleted it; try again, or save to another disk.";
    case fnfErr: return "The file isn't there any more.";
    default: return "A disk error stopped the transfer.";
    }
}
