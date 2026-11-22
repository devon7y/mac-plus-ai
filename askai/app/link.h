/* Ask AI: the link to the relay (Hayes modem dialling, then the line protocol in relay/PROTOCOL.md). */
#ifndef ASKAI_LINK_H
#define ASKAI_LINK_H
#include <MacTypes.h>

/* settings, saved in "Ask AI Prefs" in the System Folder (layout shared with tools/prefs.py) */
typedef struct {
    long magic;              /* 'AskP' */
    short version;           /* 1 */
    short baud;              /* index into serBauds (3 = 19200) */
    short flow;              /* FLOW_NONE, FLOW_XON, FLOW_HW */
    short port;              /* 0 modem port, 1 printer port */
    short autoConnect;       /* dial when Ask AI starts */
    short reserved;
    unsigned char dial[64];  /* Str63: "host:port", or a whole "AT..." command, or "DIRECT" */
    unsigned char init[64];  /* Str63: sent before dialling, e.g. "ATE0V1" */
    unsigned char pin[32];   /* Str31 */
    unsigned char model[32]; /* Str31: id of the model last chosen */
    unsigned char system[256];   /* Str255: system prompt ("" = the relay's default) */
} Prefs;
#define PREFS_MAGIC 0x41736B50UL   /* 'AskP' */
extern Prefs prefs;

enum {
    L_OFF,                   /* port closed */
    L_WAKE,                  /* "AT", waiting for OK */
    L_ESC1, L_ESC2, L_ESC3,  /* the modem seemed online: guard, "+++", ATH */
    L_INIT,                  /* init string, waiting for OK */
    L_DIAL,                  /* ATDT, waiting for CONNECT */
    L_GREET,                 /* connected, waiting for the relay's ASKAI line */
    L_HELLO,                 /* HELLO sent */
    L_SETUP,                 /* SYSTEM / MODELS / USE */
    L_READY,                 /* idle: messages can be sent */
    L_REPLY,                 /* a reply is streaming */
    L_BYE,                   /* BYE sent, waiting for NO CARRIER */
    L_HESC1, L_HESC2, L_HESC3, /* hanging up by hand: guard, "+++", ATH */
    L_TERM                   /* modem terminal: typed lines go straight to the modem */
};
extern short linkState;

typedef struct { char id[32]; char tag[9]; char label[41]; } Model;
#define MAXMODELS 24
extern Model models[MAXMODELS];
extern short nModels, curModel;      /* curModel: index, -1 if none */
extern long replyBytes, replyTicks;  /* the reply being streamed (or the last one) */
extern unsigned long replyStart;

void link_connect(void);
void link_terminal(void);            /* open the port as a plain terminal to the modem */
void link_termSend(const char *s, short n);
void link_disconnect(void);
void link_poll(void);
int link_online(void);               /* connected to the relay (GREET ... REPLY) */
int link_ask(const char *text, short n);   /* 1 sent, 0 not possible now */
void link_stop(void);
void link_newChat(void);
void link_use(short idx);
void link_sendSystem(void);
const char *link_stateName(void);

/* implemented by the user interface (main.c) */
void ui_note(const char *s);
void ui_error(short code, const char *s);
void ui_beginReply(const char *tag);
void ui_text(const char *s, short n);
void ui_newline(void);
void ui_endReply(const char *stop);
void ui_modelsChanged(void);
void ui_stateChanged(void);
void ui_rxBytes(long n);
void ui_termText(const char *s, short n);   /* raw bytes from the modem in terminal mode */
void ui_idle(void);                  /* lets the UI breathe during long waits */

enum { E_NONE, E_PORT, E_NOMODEM, E_INIT, E_DIAL, E_NORELAY, E_PIN, E_LOST, E_RELAY, E_MODEL };

#endif
