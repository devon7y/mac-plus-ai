/* Blockworld: a sound effect part (synth.h); the reference for synth.s. Frequency and amplitude
 * step every 32 samples (STEP, 1.4 ms), so the inner loop only adds the phase. */
#include "synth.h"

#define RATE 22257
#define STEP 32

void synth_part_c(unsigned char *out, long len, long f, long df, long a, long da, long kind, unsigned short *lfsr)
{
    long i;
    short hi = 1;
    unsigned short ph = 0, hold = 0;
    unsigned char v = 128;
    df *= STEP; da *= STEP;
    for (i = 0; i < len; i += STEP, f += df, a += da) {
        short fi = (short)(f >> 16), ai = (short)(a >> 16), k, n = (short)(len - i < STEP ? len - i : STEP);
        for (k = 0; k < n; k++) {
            unsigned char s = 128;
            if (kind == 1 && fi > 0) {
                ph = (unsigned short)(ph + fi * 2);                      /* a flip each half period */
                if (ph >= RATE) { ph -= RATE; hi = (short)!hi; }
                s = (unsigned char)(hi ? 128 + ai : 128 - ai);
            } else if (kind == 2 && fi > 0) {
                hold = (unsigned short)(hold + fi);
                if (hold >= RATE) {
                    hold -= RATE;
                    *lfsr = (unsigned short)((*lfsr >> 1) ^ ((*lfsr & 1) ? 0xB400 : 0));
                    v = (unsigned char)(128 + (((short)((*lfsr & 255) - 128) * ai) >> 7));
                }
                s = v;
            }
            *out++ = s;
        }
    }
}
