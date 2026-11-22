/* Shuttle: the line to the server. Hayes modem dialling (from Ask AI's link.c), then the byte stream
   split into text lines and CRC-checked frames (../PROTOCOL.md section 1). */
#ifndef SHUTTLE_LINK_H
#define SHUTTLE_LINK_H
#include <MacTypes.h>

/* settings, saved in "Shuttle Prefs" in the System Folder (layout shared with build.py) */
typedef struct {
    long magic;              /* 'ShtP' */
    short version;           /* 1 */
    short baud;              /* index into serBauds (3 = 19200, 6 = the hidden MAME test speed) */
    short flow;              /* FLOW_NONE, FLOW_XON, FLOW_HW */
    short port;              /* 0 modem port, 1 printer port */
    short autoConnect;       /* dial when Shuttle starts */
    short window;            /* blocks in flight (0: 4) */
    short block;             /* bytes per block (0: 1024) */
    short reserved;
    unsigned char dial[64];  /* Str63: an alias ("mac"), "host:port", a whole "AT..." command, or "DIRECT" */
    unsigned char init[64];  /* Str63: sent before dialling, e.g. "ATE0V1" */
} Prefs;
#define PREFS_MAGIC 0x53687450UL   /* 'ShtP' */
extern Prefs prefs;

enum {
    L_OFF,                   /* port closed */
    L_WAKE,                  /* "AT", waiting for OK */
    L_ESC1, L_ESC2, L_ESC3,  /* the modem seemed online: guard, "+++", ATH */
    L_INIT,                  /* init string, waiting for OK */
    L_DIAL,                  /* ATDT, waiting for CONNECT */
    L_ONLINE,                /* connected: the protocol runs (proto.c) */
    L_BYE,                   /* BYE sent, waiting for NO CARRIER */
    L_HESC1, L_HESC2, L_HESC3, /* hanging up by hand: guard, "+++", ATH */
    L_TERM                   /* modem terminal: typed lines go straight to the modem */
};
extern short linkState;
extern long linkRx, linkTx;          /* bytes received / sent since the port opened */
extern long linkBadFrames;

void link_init(void);
void link_connect(void);
void link_terminal(void);            /* open the port as a plain terminal to the modem */
void link_termSend(const char *s, short n);
void link_disconnect(void);
void link_hangupNow(void);           /* after BYE was answered: wait for NO CARRIER */
void link_poll(void);
int link_online(void);
const char *link_stateName(void);
long link_writeRoom(void);
void link_frame(unsigned char type, unsigned char id, unsigned short block, const unsigned char *p, short n);
unsigned short crc16(const unsigned char *p, long n, unsigned short crc);

/* implemented elsewhere: the protocol (proto.c) and the user interface (main.c) */
void proto_start(void);              /* connected: wait for the greeting, then HELLO */
void proto_line(const char *s);      /* a text line while online (the greeting) */
void proto_frame(unsigned char type, unsigned char id, unsigned short block, const unsigned char *p, short n);
void proto_bad(void);                /* a damaged frame */
void proto_lost(void);               /* the line is gone */
void ui_error(short code, const char *s);
void ui_stateChanged(void);
void ui_termText(const char *s, short n);   /* raw bytes from the modem in terminal mode */

enum { E_NONE, E_PORT, E_NOMODEM, E_INIT, E_DIAL, E_NOSERVER, E_LOST, E_SERVER, E_DISK, E_FILE, E_CANCEL };

#endif
