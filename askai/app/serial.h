/* Ask AI: the Serial Driver on the modem (or printer) port, asynchronous reads and writes. */
#ifndef ASKAI_SERIAL_H
#define ASKAI_SERIAL_H
#include <MacTypes.h>

enum { FLOW_NONE, FLOW_XON, FLOW_HW };
#define NBAUDS 6                        /* speeds offered in Settings */
#define BAUD_MAMETEST 6                 /* hidden: SCC time constant 15, exactly 7200 baud in MAME (see serial.c) */
extern const long serBauds[NBAUDS + 1]; /* 1200 ... 57600, then the test speed */

OSErr ser_open(short port, short baudIdx, short flow);   /* port 0 modem, 1 printer */
void ser_close(void);
int ser_isOpen(void);
long ser_read(char *dst, long max);     /* bytes received so far (never blocks) */
void ser_write(const char *s, long n);  /* queued; sent asynchronously */
int ser_writeIdle(void);                /* everything queued has gone out */
void ser_poll(void);                    /* keeps reads and writes going: call often */
void ser_setDTR(int on);
long ser_errors(void);                  /* hardware overrun / framing errors seen (cumulative) */

#endif
