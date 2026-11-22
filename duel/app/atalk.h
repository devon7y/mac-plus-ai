/* Orbit Duel: AppleTalk transport (atalk.c): .MPP, a DDP socket, NBP names. */
#ifndef ATALK_H
#define ATALK_H
#include <stdint.h>

#define AT_MAX 80                           /* largest datagram we send or keep */
#define AT_NBP_TYPE "\pOrbit Duel"          /* our NBP type: "<Mac name>:Orbit Duel@*" */

/* at_start results besides OSErrs */
#define AT_ERR_OFF (-98)                    /* portNotCf: AppleTalk is turned off */
#define AT_ERR_INUSE (-97)                  /* portInUse: the printer port is used by something else */

typedef struct AtPeer {
    uint32_t addr;                          /* net << 16 | node << 8 | socket */
    unsigned char name[34];                 /* Pascal string (the NBP object) */
} AtPeer;

extern uint32_t at_my_addr;
extern short at_version;                    /* .MPP version (SysEnvirons) */
extern AtPeer at_found[8];
extern short at_nfound;                     /* others found by the last lookup */
extern short at_self_seen;                  /* our own name came back in the last lookup */
extern short at_total;                      /* all names in the last lookup, ours included */
extern unsigned char at_my_name[34];

short at_state_off(void);                   /* 1 if SPConfig says AppleTalk is off */
short at_start(short turn_on);              /* open .MPP (turn_on: set "AppleTalk active" first) and a socket */
short at_register_begin(void);              /* register our name (asynchronous) */
short at_poll_register(void);               /* 1 busy, 0 done, < 0 error */
short at_lookup_begin(void);
short at_poll_lookup(void);                 /* 1 busy, 0 done (at_found), < 0 error */
short at_send(uint32_t addr, const unsigned char *p, short n);
short at_recv(unsigned char *p, short max, uint32_t *from);
short at_set_self_send(short on);           /* returns the old setting, or < 0 (needs .MPP 48+) */
void at_stop(void);                         /* remove the name, close the socket (.MPP stays open) */
short at_is_started(void);
#endif
