/* Blockworld: one part of a sound effect (sound.c): len samples at 22257 Hz into out; frequency
 * f and amplitude a (16.16 fixed point) step by df and da per sample. kind 0 silence, 1 a square
 * wave (a flip each half period), 2 noise held at f (the 16-bit LFSR *lfsr steps on). len < 32768.
 * (len and kind are longs: Retro68 pushes a short argument as 2 bytes.) */
#ifndef BW_SYNTH_H
#define BW_SYNTH_H

void synth_part_c(unsigned char *out, long len, long f, long df, long a, long da, long kind, unsigned short *lfsr);
#ifdef __m68k__
void synth_part_asm(unsigned char *out, long len, long f, long df, long a, long da, long kind, unsigned short *lfsr);
#define synth_part synth_part_asm
#else
#define synth_part synth_part_c
#endif

#endif
