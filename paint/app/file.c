/*
 * MacPaint documents (file type 'PNTG'): a 512-byte header (version 2, the 38 patterns,
 * padding) followed by 720 rows of 72 bytes, each row PackBits-compressed on its own.
 */
#include "doodle.h"

static unsigned char gName[64] = "\pUntitled";
static short gVRef;
static Boolean gNamed;

static void SetName(ConstStr255Param n)
{
    short i, len = n[0] > 63 ? 63 : n[0];
    gName[0] = (unsigned char)len;
    for (i = 1; i <= len; i++)
        gName[i] = n[i];
}

OSErr WritePaintFile(ConstStr255Param name, short vRef)
{
    Ptr buf, src, dst;
    long len, count;
    short ref, row, i;
    OSErr err;
    buf = NewPtr(512 + (long)PAGE_H * (PAGE_RB + 2));
    if (!buf)
        return dbg.lastErr = memFullErr;
    for (i = 0; i < 512; i++)
        buf[i] = 0;
    buf[3] = 2;                                   /* version 2: patterns included */
    for (i = 0; i < NPATS; i++)
        BlockMove(&gPats[i], buf + 4 + 8 * i, 8);
    src = gDoc.baseAddr;
    dst = buf + 512;
    for (row = 0; row < PAGE_H; row++)
        PackBits(&src, &dst, PAGE_RB);
    len = dst - buf;
    err = Create(name, vRef, 'Ddle', 'PNTG');
    if (err == dupFNErr)
        err = noErr;
    if (!err)
        err = FSOpen(name, vRef, &ref);
    if (!err) {
        err = SetEOF(ref, 0);
        count = len;
        if (!err)
            err = FSWrite(ref, &count, buf);
        FSClose(ref);
        FlushVol(NULL, vRef);
    }
    DisposePtr(buf);
    dbg.lastErr = err;
    if (!err)
        dbg.fileOps++;
    return err;
}

OSErr ReadPaintFile(ConstStr255Param name, short vRef)
{
    Ptr buf, src, end, dst, tp;
    long eof, count;
    short ref, row, i;
    char tmp[256];
    OSErr err;
    err = FSOpen(name, vRef, &ref);
    if (err)
        return dbg.lastErr = err;
    err = GetEOF(ref, &eof);
    if (!err && eof < 512)
        err = eofErr;
    buf = NULL;
    if (!err) {
        buf = NewPtr(eof);
        if (!buf)
            err = memFullErr;
    }
    if (!err) {
        count = eof;
        err = FSRead(ref, &count, buf);
    }
    FSClose(ref);
    if (err) {
        if (buf)
            DisposePtr(buf);
        return dbg.lastErr = err;
    }
    EndText();
    Deselect();
    SaveUndo();
    src = buf + 512;
    end = buf + eof;
    dst = gDoc.baseAddr;
    for (row = 0; row < PAGE_H; row++) {
        if (src >= end) {
            for (i = 0; i < PAGE_RB; i++)
                *dst++ = 0;
            continue;
        }
        tp = tmp;                                 /* a run may overshoot the row: unpack aside */
        UnpackBits(&src, &tp, PAGE_RB);
        BlockMove(tmp, dst, PAGE_RB);
        dst += PAGE_RB;
    }
    DisposePtr(buf);
    ClearPrompt();
    DrawCanvas();
    MarkOp();
    dbg.lastErr = 0;
    dbg.fileOps++;
    return noErr;
}

static Point DialogWhere(void)
{
    Point p;
    p.v = 60;
    p.h = gOX + 30;
    return p;
}

void FileNew(void)
{
    ClearCanvas();
    gNamed = false;
    SetName("\pUntitled");
    SetStatus("\pNew drawing");
}

void FileOpen(void)
{
    SFReply reply;
    SFTypeList types;
    types[0] = 'PNTG';
    SFGetFile(DialogWhere(), "\p", NULL, 1, types, NULL, &reply);
    SetPort(gWin);
    if (!reply.good)
        return;
    if (ReadPaintFile(reply.fName, reply.vRefNum) == noErr) {
        SetName(reply.fName);
        gVRef = reply.vRefNum;
        gNamed = true;
        SetStatus(gName);
    } else {
        SysBeep(10);
        SetStatus("\pCould not open that file.");
    }
}

void FileSave(Boolean saveAs)
{
    SFReply reply;
    if (saveAs || !gNamed) {
        SFPutFile(DialogWhere(), "\pSave drawing as:", gName, NULL, &reply);
        SetPort(gWin);
        if (!reply.good)
            return;
        SetName(reply.fName);
        gVRef = reply.vRefNum;
    }
    if (WritePaintFile(gName, gVRef) == noErr) {
        gNamed = true;
        SetStatus("\pSaved.");
    } else {
        SysBeep(10);
        SetStatus("\pCould not save (disk full or locked?).");
    }
}
