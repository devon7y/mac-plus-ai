/* Tune -> ABC text (Save). */
#ifndef ABC_H
#define ABC_H

typedef struct AbcInfo {
    unsigned long seed;
    const char *title, *type_name, *key_name;   /* C strings */
    short meter_num, meter_den, mode;
    short tonic_li, tonic_oct, tonic_midi;
    signed char sig[7];
} AbcInfo;

/* writes the tune's ABC text (CR line ends) into buf; returns its length */
long abc_write(char *buf, long cap, const unsigned char *toks, short n, const AbcInfo *k);

#endif
