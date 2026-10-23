/* Blockworld: a small byte-aligned LZ77 codec for the save file (format "BWLZ", like LZ4's block
 * format). A token byte holds the literal count (high nibble) and the match length - 3 (low
 * nibble); 15 in either means more length bytes follow (each added, 255 = keep going); then the
 * literals; then, unless the output is complete, the match offset (2 bytes, big-endian, 1..65535).
 * Matches may overlap their own output (runs). tools/bwsave.py is the host reference. */
#ifndef BW_LZ_H
#define BW_LZ_H

#define LZ_MINM 3
#define LZ_HBITS 12                       /* the packer's hash table: 4096 positions */
/* pack n bytes from in into out (room for lz_bound(n)); work: lz_work(n) bytes. Greedy, with the
   last position of each hash as the one candidate, positions inside matches not remembered (fast
   on the 68000; tools/bwsave.py packs the shipped world tighter). Returns the packed length. */
long lz_pack_c(const unsigned char *in, long n, unsigned char *out, void *work);
#define lz_bound(n) ((n) + (n) / 255 + 16)
#define lz_work(n) (4L << LZ_HBITS)              /* (the hash table) */

/* unpack into out (exactly n bytes) from in (len bytes). Returns 0, or -1 if the data is bad.
   The 68000 version may write up to LZ_SLACK bytes past out + n and read up to LZ_SLACK past
   in + len (bad data only): give both buffers that much room. */
#define LZ_SLACK 32
short lz_unpack_c(const unsigned char *in, long len, unsigned char *out, long n);
#ifdef __m68k__
short lz_unpack_asm(const unsigned char *in, long len, unsigned char *out, long n);
long lz_pack_asm(const unsigned char *in, long n, unsigned char *out, void *work);   /* lz.s: the same */
#define lz_unpack lz_unpack_asm
#define lz_pack lz_pack_asm
#else
#define lz_unpack lz_unpack_c
#define lz_pack lz_pack_c
#endif

#endif
