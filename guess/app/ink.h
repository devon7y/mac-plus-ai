/* Guess My Doodle: mouse samples from a VBL task (ink.c). */
#ifndef GUESS_INK_H
#define GUESS_INK_H

int ink_init(void);                             /* 1 if the VBL task is installed */
void ink_quit(void);
int ink_next(short *v, short *h, short *down);  /* the next sample (global coordinates), 0 if none */
void ink_flush(void);                           /* forget the samples so far */
int ink_pending(void);                          /* 1 if samples are waiting */

#endif
