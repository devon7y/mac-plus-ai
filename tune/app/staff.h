/* Tune's staff: the written tune as events, laid out four bars to a system, drawn with QuickDraw. */
#ifndef STAFF_H
#define STAFF_H
#include <Quickdraw.h>

#define ST_MAXEV 1400
#define ST_MAXSLOT 48

enum { EV_NOTE = 1, EV_REST, EV_BAR };

typedef struct Ev {
    unsigned char kind, tok;   /* tok: the bar token for EV_BAR (BAR..E2, EOS) */
    signed char deg, alt;      /* notes: degree from the tonic, alteration -1/0/1 */
    unsigned char dur, real;   /* nominal / sounding ticks */
    unsigned char pos;         /* bar position (ticks) where it starts */
    unsigned char trip;        /* 1..3 inside a triplet, else 0 */
    unsigned char acc;         /* accidental drawn: 0, 1 sharp, 2 flat, 3 natural */
    unsigned char midi;
    short slot;
    short x, y;                /* notehead (left, centre line), set when drawn */
} Ev;

typedef struct Staff {
    Ev ev[ST_MAXEV];
    short nev;
    short slot_sys[ST_MAXSLOT], slot_x0[ST_MAXSLOT], slot_x1[ST_MAXSLOT], slot_ev0[ST_MAXSLOT];
    short nslot;               /* slots used (the current one is nslot - 1) */
    short pickup;              /* ticks of the opening pickup bar (0 none) */
    short blen, meter_num, meter_den;
    short tonic_li, tonic_oct, tonic_midi, mode;   /* key: letter index, letter octave, midi, mode */
    signed char sig[7];        /* key signature alteration per letter */
    short nsig;                /* sharps (>0) or flats (<0) */
    Rect area;                 /* where the systems go */
    short sp, sys_h, nsys_max;
    short highlight;           /* event index shown inverted, or -1 */
    short open_g0;             /* first note of the beam group still open in the current slot */
} Staff;

void st_reset(Staff *s, const Rect *area, short meter_num, short meter_den,
              short tonic_li, short tonic_oct, short tonic_midi, short mode, short nsig, const signed char *sig);
/* add an event at the current position (slot handling included); returns its index */
short st_add(Staff *s, const Ev *e, short new_slot_after);
void st_draw_all(Staff *s);           /* erase the area and draw everything */
void st_draw_slot(Staff *s, short k); /* redraw one bar slot */
void st_highlight(Staff *s, short ev); /* move the playing-note highlight */
short st_note_midi(const Staff *s, short deg, short alt);
void st_letter(const Staff *s, short deg, short *letter, short *octave);
#endif
