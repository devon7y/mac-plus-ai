/* Tune's player: performance lists (repeats expanded), a simple bass accompaniment, and the
   interrupt-time sequencer and mixer (VBL + Time Manager tasks writing the sound buffer). */
#ifndef PLAYER_H
#define PLAYER_H

#define PL_MAXP 1200            /* notes per voice in performance order */

typedef struct PNote {
    short dur;                  /* ticks (1/48 of a whole note); sounding length (triplets: 2/3) */
    unsigned char midi;         /* 0 = rest */
    unsigned char pad;
    short wnote;                /* written note index (melody), -1 for none */
} PNote;

/* main program side */
short pl_init(void);            /* tables, tasks; 0 ok */
void pl_quit(void);
void pl_reset(void);            /* empty the lists (stops playing) */
void pl_set_tempo(short bpm, short beat_ticks);
void pl_set_sound(short on);    /* M: off silences the output, the lists stay */
void pl_set_timbre(short t);    /* 0 fiddle, 1 flute */
void pl_set_bass(short on);
short pl_start(void);           /* play from the beginning; 0 if nothing to play */
void pl_stop(void);
short pl_playing(void);         /* 1 while playing (also while waiting for more notes) */
short pl_cur_note(void);        /* written index of the melody note sounding now, or -1 */
long pl_ready_ticks(void);      /* performance ticks complete so far */
long pl_pos_ticks(void);        /* performance position of the player */
void pl_debug(unsigned long *out);   /* 6 longs */

/* appending (main program; the interrupt task reads up to the counts) */
void pl_add_melody(short dur, unsigned char midi, short wnote);
void pl_add_bass(short dur, unsigned char midi);
void pl_mark_ready(void);       /* everything appended so far may be played */
short pl_nmel(void);
short pl_nbas(void);
void pl_copy(short m0, short m1, short b0, short b1);   /* append copies of these ranges (repeats) */
void pl_finished(void);         /* no more notes: the player stops at the end */

#endif
