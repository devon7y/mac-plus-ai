/* Blockworld: saving and loading the world.
 *
 * Files next to the game: "Blockworld World" (the player's world, written on save) and
 * "Blockworld Default" (the shipped starting world, locked; read when there is no world yet and
 * for "Starting world" in the menu). tools/bwsave.py reads and writes them on the host.
 *
 * Format 7: a header ('BWSV', 7, the number of blocks, the state), a second header (the payload's
 * length unpacked and packed, its checksum, the sizes of the saved arrays), then the payload,
 * LZ-packed (lz.c): the arrays (inventory, armour, chests, furnaces, objects, mobs, dropped items)
 * and the world as a dictionary of its distinct columns (world.h; about 2,500 of 16,384), about
 * 35 KB for a new world. Loading reads it in one go, builds each distinct column's blocks and
 * segment list once and copies the rest (no scan of the blocks: unpack.s).
 * Formats 4-6 (the blocks run-length coded as (count, type) byte pairs) still load. */
#include <Files.h>
#include <MacMemory.h>
#include <string.h>
#include "save.h"
#include "world.h"
#include "obj.h"
#include "mob.h"
#include "drop.h"
#include "lz.h"

#define SAVE_MAGIC 0x42575356L    /* 'BWSV' */
#define SAVE_VERSION 7            /* 5: + the mobs, 6: + dropped items, 7: LZ-packed runs */
#define CHUNK 4096
#define NSEC 7
#define FTYPE 0x42577764L         /* 'BWwd': a Blockworld world (its Finder icon: toolchain/icons) */
#define FCREATOR 0x42577264L      /* 'BWrd' (the game) */

unsigned char save_name[] = "\pBlockworld World";
unsigned char default_name[] = "\pBlockworld Default";
unsigned long save_t[6];          /* tests: ticks: load read, unpack, world; save pack, LZ, write */
#define TICKS (*(volatile unsigned long *)0x16A)

typedef struct Header {           /* every version */
    long magic, version, nblocks;
    SaveState st;
} Header;
typedef struct Header7 {          /* then, from version 7 */
    long rawlen, packlen, sum;
    long size[NSEC];              /* the arrays' sizes (a build with other sizes can't read it) */
} Header7;

static void sections(void **p, long *n)
{
    p[0] = inv; n[0] = sizeof inv;
    p[1] = inv_arm; n[1] = sizeof inv_arm;
    p[2] = chests; n[2] = sizeof chests;
    p[3] = furns; n[3] = sizeof furns;
    p[4] = objs; n[4] = sizeof objs;
    p[5] = mobs; n[5] = sizeof mobs;
    p[6] = drops; n[6] = sizeof drops;
}

/* the payload's checksum: rotate left one bit, add the next big-endian long (zero-padded) */
unsigned long save_sum(const unsigned char *p, long n)
{
    unsigned long s = 0, v;
    long i;
    for (i = 0; i + 4 <= n; i += 4) {
        v = *(const unsigned long *)(p + i);
        s = ((s << 1) | (s >> 31)) + v;
    }
    if (i < n) {
        v = 0;
        for (; i < n; i++) v |= (unsigned long)p[i] << (24 - 8 * (i & 3));
        s = ((s << 1) | (s >> 31)) + v;
    }
    return s;
}

static short fwrite_(short ref, const void *p, long n)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)p;
    pb.ioParam.ioReqCount = n;
    pb.ioParam.ioPosMode = fsAtMark;
    return PBWriteSync(&pb);
}

static short fread_(short ref, void *p, long n, long *got)
{
    ParamBlockRec pb;
    short err;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    pb.ioParam.ioBuffer = (Ptr)p;
    pb.ioParam.ioReqCount = n;
    pb.ioParam.ioPosMode = fsAtMark;
    err = PBReadSync(&pb);
    *got = pb.ioParam.ioActCount;
    return err;
}

static void fclose_(short ref, short flush)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioRefNum = ref;
    PBCloseSync(&pb);
    if (flush) {
        memset(&pb, 0, sizeof pb);
        PBFlushVolSync(&pb);
    }
}

static unsigned char *chunk;      /* (format 6: written in pieces through it) */
unsigned char *save_test_buf;     /* tests: bytes for save_test to write (mode 5 and up; 64 KB) */

/* the old format (6): if the new one doesn't fit in memory */
static short save_v6(short ref, Header *h)
{
    long i, n = (long)WX * WX * WY, k = 0;
    short err;
    unsigned char buf[512];
    h->version = 6;
    chunk = buf;
    err = fwrite_(ref, h, sizeof *h);
    if (!err) err = fwrite_(ref, inv, sizeof inv);
    if (!err) err = fwrite_(ref, inv_arm, sizeof inv_arm);
    if (!err) err = fwrite_(ref, chests, sizeof chests);
    if (!err) err = fwrite_(ref, furns, sizeof furns);
    if (!err) err = fwrite_(ref, objs, sizeof objs);
    if (!err) err = fwrite_(ref, mobs, sizeof mobs);
    if (!err) err = fwrite_(ref, drops, sizeof drops);
    for (i = 0; i < n && !err;) {
        unsigned char t = bw_blocks[i];
        long r = 1;
        while (i + r < n && r < 255 && bw_blocks[i + r] == t) r++;
        chunk[k++] = (unsigned char)r;
        chunk[k++] = t;
        i += r;
        if (k >= (long)sizeof buf - 2 || i >= n) {
            err = fwrite_(ref, chunk, k);
            k = 0;
        }
    }
    return err;
}

/* create (with the game's type and creator: its icon) and open a file, empty */
static short open_new(const unsigned char *name, short *ref)
{
    ParamBlockRec pb;
    short err;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    PBCreateSync(&pb);                              /* (fails harmlessly if it exists) */
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    if (PBGetFInfoSync(&pb) == noErr && (pb.fileParam.ioFlFndrInfo.fdType != FTYPE
                                         || pb.fileParam.ioFlFndrInfo.fdCreator != FCREATOR)) {
        pb.fileParam.ioFlFndrInfo.fdType = FTYPE;
        pb.fileParam.ioFlFndrInfo.fdCreator = FCREATOR;
        pb.fileParam.ioNamePtr = (StringPtr)name;
        pb.fileParam.ioFDirIndex = 0;
        PBSetFInfoSync(&pb);
    }
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioPermssn = fsWrPerm;
    if ((err = PBOpenSync(&pb)) != 0) return err;
    *ref = pb.ioParam.ioRefNum;
    memset(&pb, 0, sizeof pb);                      /* from the start, truncated */
    pb.ioParam.ioRefNum = *ref;
    pb.ioParam.ioMisc = 0;
    PBSetEOFSync(&pb);
    return 0;
}

static short delete_file(const unsigned char *name)
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = (StringPtr)name;
    return PBDeleteSync(&pb);
}

/* the whole file: the headers and the packed payload (or, pk 0, format 6); 0 or the error */
static short write_save(const unsigned char *name, Header *h, Header7 *h7, const unsigned char *pk)
{
    short ref, err;
    long i;
    if ((err = open_new(name, &ref)) != 0) return err;
    if (!pk)
        err = save_v6(ref, h);
    else {
        err = fwrite_(ref, h, sizeof *h);
        if (!err) err = fwrite_(ref, h7, sizeof *h7);
        for (i = 0; !err && i < h7->packlen; i += CHUNK)          /* (in pieces, as format 6 did) */
            err = fwrite_(ref, pk + i, h7->packlen - i < CHUNK ? h7->packlen - i : CHUNK);
    }
    fclose_(ref, 1);
    return err;
}

unsigned char temp_name[] = "\pBlockworld Saving";
short save_tries;                 /* tests: the attempts the last save took */

/* Everything is packed first; then the file is written as "Blockworld Saving" (up to three tries:
   the SE's 1.44 MB floppy fails now and then in MAME, error -36) and only then takes the place of
   "Blockworld World", so a failed save never harms the world saved before. */
short save_world(const SaveState *st)
{
    Header h;
    Header7 h7;
    void *sp[NSEC];
    long sn[NSEC], i, off, wlen;
    unsigned char *raw = 0, *pk = 0;
    void *ht = 0, *work = 0;
    short err;
    unsigned long t0 = TICKS;
    sections(sp, sn);
    h.magic = SAVE_MAGIC; h.version = SAVE_VERSION; h.nblocks = (long)WX * WX * WY;
    h.st = *st;
    memset(&h7, 0, sizeof h7);
    for (i = 0, off = 0; i < NSEC; i++) { h7.size[i] = sn[i]; off += sn[i]; }
    work = NewPtr(WORLD_WORK);
    wlen = work ? world_pack(0, work) : 0;           /* (the dictionary of columns, in work; room) */
    raw = wlen ? (unsigned char *)NewPtr(off + wlen) : 0;
    if (raw) {
        h7.rawlen = off + world_pack(raw + off, work);
        DisposePtr((Ptr)work);
        work = 0;
        pk = (unsigned char *)NewPtr(lz_bound(h7.rawlen));
    }
    if (pk) ht = NewPtr(lz_work(h7.rawlen));
    if (ht) {
        for (i = 0, off = 0; i < NSEC; i++) { memcpy(raw + off, sp[i], sn[i]); off += sn[i]; }
        h7.sum = (long)save_sum(raw, h7.rawlen);
        save_t[3] = TICKS - t0; t0 = TICKS;
        h7.packlen = lz_pack(raw, h7.rawlen, pk, ht);
        save_t[4] = TICKS - t0; t0 = TICKS;
    } else if (pk) {                                /* (no room to pack: the old format) */
        DisposePtr((Ptr)pk);
        pk = 0;
    }
    if (work) DisposePtr((Ptr)work);
    if (raw) DisposePtr((Ptr)raw);
    if (ht) DisposePtr((Ptr)ht);
    for (save_tries = 1; ; save_tries++) {
        err = write_save(temp_name, &h, &h7, pk);
        if (!err || save_tries == 3) break;
    }
    if (!err) {                                     /* the new file takes the old one's place */
        ParamBlockRec pb;
        delete_file(save_name);
        memset(&pb, 0, sizeof pb);
        pb.ioParam.ioNamePtr = temp_name;
        pb.ioParam.ioMisc = (Ptr)save_name;
        err = PBRenameSync(&pb);
        memset(&pb, 0, sizeof pb);
        PBFlushVolSync(&pb);
    } else
        delete_file(temp_name);
    if (pk) DisposePtr((Ptr)pk);
    save_t[5] = TICKS - t0;
    return err;
}

/* formats 4-6: the arrays, then the run-length coded blocks */
static short load_v6(short ref, const Header *h, void (*prepare)(void))
{
    long got, i = 0, n = (long)WX * WX * WY;
    unsigned char *buf = (unsigned char *)NewPtr(CHUNK);
    if (!buf) return memFullErr;
    fread_(ref, inv, sizeof inv, &got);
    fread_(ref, inv_arm, sizeof inv_arm, &got);
    fread_(ref, chests, sizeof chests, &got);
    fread_(ref, furns, sizeof furns, &got);
    fread_(ref, objs, sizeof objs, &got);
    obj_reindex();
    memset(mobs, 0, sizeof mobs);
    if (h->version >= 5) fread_(ref, mobs, sizeof mobs, &got);
    memset(drops, 0, sizeof drops);
    if (h->version >= 6) fread_(ref, drops, sizeof drops, &got);
    while (i < n) {
        long k;
        fread_(ref, buf, CHUNK, &got);
        if (got <= 0) break;
        for (k = 0; k + 1 < got && i < n; k += 2) {
            long r = buf[k];
            unsigned char t = buf[k + 1];
            if (r > n - i) r = n - i;
            memset(bw_blocks + i, t, r);
            i += r;
        }
    }
    DisposePtr((Ptr)buf);
    if (i < n) return -1;
    if (prepare) prepare();
    world_build_segments();
    return 0;
}

short load_world(SaveState *st, const unsigned char *name, void (*prepare)(void))
{
    ParamBlockRec pb;
    Header h;
    Header7 h7;
    void *sp[NSEC];
    long sn[NSEC], got, i, off;
    unsigned char *raw = 0, *pk = 0;
    short ref, err;
    unsigned long t0 = TICKS;
    memset(save_t, 0, 3 * sizeof save_t[0]);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = (StringPtr)name;
    pb.ioParam.ioPermssn = fsRdPerm;
    if ((err = PBOpenSync(&pb)) != 0) return err;
    ref = pb.ioParam.ioRefNum;
    err = fread_(ref, &h, sizeof h, &got);
    if (err || got != sizeof h || h.magic != SAVE_MAGIC || h.version > SAVE_VERSION || h.version < 4
        || h.nblocks != (long)WX * WX * WY) {
        fclose_(ref, 0);
        return err ? err : -1;
    }
    if (h.version < 7) {
        err = load_v6(ref, &h, prepare);
        fclose_(ref, 0);
        if (!err) *st = h.st;
        return err;
    }
    sections(sp, sn);
    err = fread_(ref, &h7, sizeof h7, &got);
    for (i = 0, off = 0; i < NSEC; i++) {
        if (h7.size[i] != sn[i]) err = -2;          /* saved by a build with other arrays */
        off += sn[i];
    }
    if (!err && (got != sizeof h7 || h7.rawlen <= off || h7.rawlen > 4000000L || h7.packlen <= 0
                 || h7.packlen > lz_bound(h7.rawlen)))
        err = -1;
    if (!err) {
        pk = (unsigned char *)NewPtr(h7.packlen + LZ_SLACK);           /* (room past the ends: lz.h, */
        raw = pk ? (unsigned char *)NewPtr(h7.rawlen + LZ_SLACK + 2 * WY + 2) : 0;   /*  world.h) */
        if (!raw) err = memFullErr;
    }
    if (!err) {                                     /* the whole payload in one read */
        err = fread_(ref, pk, h7.packlen, &got);
        if (!err && got != h7.packlen) err = -1;
    }
    fclose_(ref, 0);
    save_t[0] = TICKS - t0; t0 = TICKS;
    if (!err && (lz_unpack(pk, h7.packlen, raw, h7.rawlen) || save_sum(raw, h7.rawlen) != (unsigned long)h7.sum))
        err = -1;                                   /* (nothing changed yet) */
    save_t[1] = TICKS - t0; t0 = TICKS;
    if (!err) {
        for (i = 0, off = 0; i < NSEC; i++) { memcpy(sp[i], raw + off, sn[i]); off += sn[i]; }
        obj_reindex();
        if (prepare) prepare();
        err = world_unpack(raw + off, h7.rawlen - off);
    }
    if (raw) DisposePtr((Ptr)raw);
    if (pk) DisposePtr((Ptr)pk);
    save_t[2] = TICKS - t0;
    if (!err) *st = h.st;
    return err;
}

/* tests: write a file "Blockworld Test" of n bytes in pieces of piece bytes (heads: 0 a pattern,
   1 the same after 64- and 40-byte writes, 2 zeros, 3 0xFF, 4 noise, 5 save_test_buf's bytes,
   6 and up: those with heads - 5 ticks between the pieces); 0 or the error */
short save_test(long n, long piece, short heads)
{
    static unsigned char tname[] = "\pBlockworld Test";
    ParamBlockRec pb;
    short ref, err;
    long i;
    unsigned char *buf = (unsigned char *)NewPtrClear(n);
    if (!buf) return memFullErr;
    memset(&pb, 0, sizeof pb);
    pb.fileParam.ioNamePtr = tname;
    PBCreateSync(&pb);
    memset(&pb, 0, sizeof pb);
    pb.ioParam.ioNamePtr = tname;
    pb.ioParam.ioPermssn = fsWrPerm;
    if ((err = PBOpenSync(&pb)) != 0) { DisposePtr((Ptr)buf); return err; }
    ref = pb.ioParam.ioRefNum;
    short pause = heads >= 6 ? (short)(heads - 5) : 0;   /* 6, 7, ...: those bytes, 1, 2 ... ticks between pieces */
    if (heads >= 5) {                              /* 5: the bytes tests put at save_test_buf */
        if (!save_test_buf) { DisposePtr((Ptr)buf); fclose_(ref, 0); return memFullErr; }
        memcpy(buf, save_test_buf, n);
        heads = 0;
    } else
    for (i = 0; i < n; i++)                        /* heads 2: zeros, 3: 0xFF, 4: random */
        buf[i] = heads == 2 ? 0 : heads == 3 ? 0xFF
               : heads == 4 ? (unsigned char)((i * 1103515245UL + 12345) >> 16) : (unsigned char)(i * 7);
    if (heads > 1) heads = 0;
    err = 0;
    if (heads) {                                       /* (as save_world: 64 and 40 bytes first) */
        err = fwrite_(ref, buf, 64);
        if (!err) err = fwrite_(ref, buf, 40);
    }
    for (i = 0; !err && i < n; i += piece) {
        unsigned long t = TICKS;
        err = fwrite_(ref, buf + i, n - i < piece ? n - i : piece);
        while (TICKS - t < (unsigned long)pause) ;
    }
    fclose_(ref, 1);
    DisposePtr((Ptr)buf);
    return err;
}
