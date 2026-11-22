/* Orbit Duel: the serial cable transport (serial.c). */
#ifndef SERLINK_H
#define SERLINK_H
#define SER_MAX 64                      /* largest payload */
short ser_open(short test_speed);       /* 0 = 19,200 baud; 1 = the MAME test speed (see serial.c) */
void ser_close(void);
short ser_is_open(void);
short ser_send(const unsigned char *p, short n);   /* 0, or -1 if the line is busy */
short ser_recv(unsigned char *p, short max);       /* a payload's length, or 0 */
extern long ser_bad;
#endif
