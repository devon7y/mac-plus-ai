/* Shuttle: the session with the server and the transfers (../PROTOCOL.md sections 2 and 3). */
#ifndef SHUTTLE_PROTO_H
#define SHUTTLE_PROTO_H
#include <MacTypes.h>

enum { P_OFF, P_GREET, P_IDLE, P_CMD, P_RECV, P_DONEWAIT, P_SEND, P_SAVEWAIT };
enum { J_NONE, J_HELLO, J_LIST, J_GET, J_PUT, J_CANCEL, J_BYE };
extern short pState, pJob;
extern long pDone, pTotal;           /* bytes of the current (or last) transfer */
extern unsigned long pStart, pEnd;   /* its TickCount at the start and the end */
extern char serverName[32];
extern short pWindow, pBlock;
extern long stNaks, stTimeouts, stFlushes, stResends, stBlocks, stLateFrames;
extern short dlVerifyFailed;         /* the last download didn't read back correctly (kept on disk) */

int proto_list(const char *path);                       /* 1 if the request was sent */
int proto_get(const char *path, long dlen, long rlen);  /* after dl_begin() */
int proto_put(const unsigned char *name, long total);   /* after ul_begin() */
void proto_cancel(void);
void proto_disconnect(void);
void proto_poll(void);
int proto_ready(void);               /* online and idle: a new request can start */
int proto_busy(void);                /* a listing, download or upload is running */

/* implemented by the user interface (main.c) */
void ui_connected(void);
void ui_listing(const char *text, long n);
void ui_note(const char *s);
void ui_transferDone(short job, const char *msg);
void ui_transferFailed(short job, short code, const char *msg);

#endif
