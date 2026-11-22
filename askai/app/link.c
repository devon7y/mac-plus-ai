/*
 * Ask AI: the link to the relay.
 *
 * 1. Dialling (Hayes command set, as on Zimodem / WiModem232 / WiFi232 WiFi modems): "AT" until the
 *    modem says OK (if it doesn't, it may still be online from last time: guard time, "+++", ATH,
 *    and try again), then the init string, then ATDT host:port and wait for CONNECT. Echo and
 *    numeric result codes are tolerated. A dial string starting with "AT" is sent as it is;
 *    "DIRECT" skips the modem (a plain serial line to the relay).
 * 2. The relay's line protocol (relay/PROTOCOL.md): ASKAI greeting, HELLO with the PIN (and the old
 *    session id after a dropped line), SYSTEM, MODELS, USE; then ASK and the streamed reply (BEGIN,
 *    T, N, END), with NOTE / ERR / BYE at any time. "NO CARRIER" from the modem means the line is gone.
 * 3. Hanging up: BYE (the relay hangs up, the modem says NO CARRIER), else guard, "+++", ATH; then
 *    the port is closed (which also drops DTR).
 * Everything is non-blocking: link_poll() is called from the event loop.
 */
#include <OSUtils.h>
#include <Events.h>
#include <string.h>
#include "serial.h"
#include "link.h"

Prefs prefs;
short linkState;
Model models[MAXMODELS];
short nModels, curModel = -1;
long replyBytes, replyTicks;
unsigned long replyStart;

static unsigned long stateTick;     /* TickCount when the current state began */
static short tries, escTried, direct, setupStep, discard, everConnected, userHangup;
static char line[260];
static short lineLen;
static char sessId[12];
static char dialShown[64];

static void sendHello(void);

static void enter(short s)
{
    linkState = s;
    stateTick = TickCount();
    ui_stateChanged();
}

static unsigned long since(void) { return TickCount() - stateTick; }

static void sendStr(const char *s) { ser_write(s, (long)strlen(s)); }

static void sendLine(const char *cmd, const char *arg, short n)   /* "CMD arg\r" */
{
    ser_write(cmd, (long)strlen(cmd));
    if (arg) {
        ser_write(" ", 1);
        ser_write(arg, n);
    }
    ser_write("\r", 1);
}

static void p2c(const unsigned char *p, char *c, short max)
{
    short n = p[0] < max - 1 ? p[0] : (short)(max - 1);
    memcpy(c, p + 1, n);
    c[n] = 0;
}

static int startsWith(const char *s, const char *pre) { return !strncmp(s, pre, strlen(pre)); }

static int eqi(const char *a, const char *b)       /* case-insensitive equality (ASCII) */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------ hanging up */
static void closeAll(void)
{
    ser_close();
    if (linkState == L_REPLY) ui_endReply("lost");
    enter(L_OFF);
    if (userHangup) { userHangup = 0; ui_note("(Disconnected.)"); }
}

static void fail(short code, const char *msg)
{
    userHangup = 0;
    ui_error(code, msg);
    closeAll();
}

void link_disconnect(void)
{
    if (linkState != L_OFF) userHangup = 1;
    switch (linkState) {
    case L_OFF:
        return;
    case L_GREET: case L_HELLO: case L_SETUP: case L_READY: case L_REPLY:
        if (direct) { sendLine("BYE", NULL, 0); closeAll(); return; }
        sendLine("BYE", NULL, 0);
        enter(L_BYE);
        return;
    case L_BYE: case L_HESC1: case L_HESC2: case L_HESC3:
        closeAll();                     /* asked twice: just drop the line */
        return;
    default:                            /* still talking to the modem, or the terminal */
        if (linkState == L_DIAL) sendStr("\r");   /* any character aborts a dial */
        closeAll();
        return;
    }
}

void link_terminal(void)
{
    OSErr e;
    if (linkState != L_OFF) return;
    e = ser_open(prefs.port, prefs.baud, prefs.flow);
    if (e != noErr) {
        ui_error(E_PORT, prefs.port ? "Ask AI couldn't open the printer port. If AppleTalk is on, turn it off in the Chooser."
                                    : "Ask AI couldn't open the modem port. Is another program using it?");
        return;
    }
    enter(L_TERM);
}

void link_termSend(const char *s, short n)
{
    if (linkState != L_TERM) return;
    ser_write(s, n);
    if (!(n == 3 && !memcmp(s, "+++", 3))) ser_write("\r", 1);   /* "+++" alone: the escape, no Return */
}

int link_online(void) { return linkState >= L_GREET && linkState <= L_REPLY; }

/* ------------------------------------------------------------------ connecting */
void link_connect(void)
{
    OSErr e;
    char d[64];
    if (linkState != L_OFF) return;
    p2c(prefs.dial, d, sizeof d);
    if (!d[0]) {
        ui_error(E_DIAL, "Enter your relay's address first: Connection > Settings\311 (for example 192.168.1.20:6800).");
        return;
    }
    e = ser_open(prefs.port, prefs.baud, prefs.flow);
    if (e != noErr) {
        static char msg[160];
        strcpy(msg, prefs.port ? "Ask AI couldn't open the printer port. If AppleTalk is on, turn it off in the Chooser."
                               : "Ask AI couldn't open the modem port. Is another program using it?");
        ui_error(E_PORT, msg);
        return;
    }
    strcpy(dialShown, d);
    direct = eqi(d, "DIRECT");
    tries = escTried = 0;
    lineLen = 0;
    nModels = 0;
    curModel = -1;
    ui_modelsChanged();
    if (direct) {
        sendHello();                    /* no modem: talk to the relay at once */
        return;
    }
    sendStr("\rAT\r");                  /* the CR first ends any half-typed command */
    enter(L_WAKE);
}

static void sendInit(void)
{
    char s[66];
    p2c(prefs.init, s, sizeof s);
    if (s[0]) {
        sendStr(s);
        sendStr("\r");
        enter(L_INIT);
    } else {
        char d[64];
        p2c(prefs.dial, d, sizeof d);
        if (startsWith(d, "AT") || startsWith(d, "at")) sendStr(d);
        else { sendStr("ATDT"); sendStr(d); }
        sendStr("\r");
        enter(L_DIAL);
    }
}

static void sendHello(void)
{
    static char a[60];
    char pin[32];
    short n;
    p2c(prefs.pin, pin, sizeof pin);
    strcpy(a, "1 ");
    strcat(a, pin[0] ? pin : "-");
    if (sessId[0]) { strcat(a, " "); strcat(a, sessId); }
    n = (short)strlen(a);
    sendLine("HELLO", a, n);
    enter(L_HELLO);
}

void link_sendSystem(void)
{
    if (!link_online() || linkState == L_REPLY) return;
    sendLine("SYSTEM", (const char *)prefs.system + 1, prefs.system[0]);
}

/* ------------------------------------------------------------------ result codes from the modem */
enum { R_NONE, R_OK, R_CONNECT, R_NOCARRIER, R_ERROR, R_NODIALTONE, R_BUSY, R_NOANSWER, R_ECHO, R_OTHER };

static int result(const char *s)
{
    if (startsWith(s, "AT") || startsWith(s, "at") || !strcmp(s, "+++")) return R_ECHO;
    if (!strcmp(s, "OK") || !strcmp(s, "0")) return R_OK;
    if (startsWith(s, "CONNECT")) return R_CONNECT;
    if (!strcmp(s, "NO CARRIER") || !strcmp(s, "3")) return R_NOCARRIER;
    if (!strcmp(s, "ERROR") || !strcmp(s, "4")) return R_ERROR;
    if (!strcmp(s, "NO DIALTONE") || !strcmp(s, "NO DIAL TONE") || !strcmp(s, "6")) return R_NODIALTONE;
    if (!strcmp(s, "BUSY") || !strcmp(s, "7")) return R_BUSY;
    if (!strcmp(s, "NO ANSWER") || !strcmp(s, "8")) return R_NOANSWER;
    if (s[0] >= '0' && s[0] <= '9') {                   /* other numeric codes: CONNECT <speed> */
        const char *p = s;
        while (*p >= '0' && *p <= '9') p++;
        if (!*p) return R_CONNECT;
    }
    return R_OTHER;
}

static void dialFailed(int r)
{
    static char msg[200];
    if (r == R_NOCARRIER || r == R_NOANSWER || r == R_BUSY) {
        strcpy(msg, "The modem couldn't reach the relay at ");
        strcat(msg, dialShown);
        strcat(msg, r == R_BUSY ? " (BUSY)." : r == R_NOANSWER ? " (NO ANSWER)." : " (NO CARRIER).");
        strcat(msg, " Is the relay running, and is the address right?");
    } else if (r == R_NODIALTONE) {
        strcpy(msg, "The modem isn't on Wi-Fi (NO DIALTONE). Join your network with the modem's own commands first.");
    } else if (r == R_ERROR) {
        strcpy(msg, "The modem rejected the dial command (ERROR). Check the address in Connection > Settings\311.");
    } else {
        strcpy(msg, "No answer from the modem after dialling ");
        strcat(msg, dialShown);
        strcat(msg, ".");
    }
    fail(E_DIAL, msg);
}

/* ------------------------------------------------------------------ relay lines */
static short findModel(const char *id)
{
    short i;
    for (i = 0; i < nModels; i++) if (!strcmp(models[i].id, id)) return i;
    return -1;
}

/* split "a b rest": copy the first word of *s into w (max n), advance *s past it and one blank */
static void word(const char **s, char *w, short n)
{
    short k = 0;
    while (**s && **s != ' ') {
        if (k < n - 1) w[k++] = **s;
        (*s)++;
    }
    w[k] = 0;
    if (**s == ' ') (*s)++;
}

static void setupNext(void)
{
    /* after HELLO: SYSTEM, MODELS (then USE if the saved model differs), then ready */
    char want[32];
    if (setupStep == 0) {
        setupStep = 1;
        sendLine("SYSTEM", (const char *)prefs.system + 1, prefs.system[0]);
        return;
    }
    if (setupStep == 1) {
        setupStep = 2;
        sendLine("MODELS", NULL, 0);
        return;
    }
    if (setupStep == 2) {
        short k;
        setupStep = 3;
        p2c(prefs.model, want, sizeof want);
        k = findModel(want);
        if (want[0] && k >= 0 && k != curModel) {
            sendLine("USE", models[k].id, (short)strlen(models[k].id));
            return;
        }
    }
    enter(L_READY);
}

static void relayLine(const char *s)
{
    char w[48];
    const char *p = s;
    if (linkState == L_REPLY) {
        if (s[0] == 'T' && s[1] == ' ') { if (!discard) ui_text(s + 2, (short)strlen(s + 2)); return; }
        if (s[0] == 'N' && !s[1]) { if (!discard) ui_newline(); return; }
    }
    word(&p, w, sizeof w);
    if (!strcmp(w, "ASKAI")) {
        if (linkState == L_GREET || linkState == L_WAKE || linkState == L_INIT || linkState == L_DIAL) sendHello();
        return;
    }
    if (!strcmp(w, "OK")) {
        word(&p, w, sizeof w);
        if (!strcmp(w, "HELLO") && linkState == L_HELLO) {
            char resumed[12], turns[8], cur[32];
            word(&p, sessId, sizeof sessId);
            word(&p, resumed, sizeof resumed);
            word(&p, turns, sizeof turns);
            word(&p, cur, sizeof cur);
            if (!strcmp(resumed, "RESUMED") && strcmp(turns, "0"))
                ui_note("(Back on line: the relay still has this conversation.)");
            else if (everConnected && strcmp(resumed, "RESUMED"))
                ui_note("(The relay started a new conversation.)");
            everConnected = 1;
            setupStep = 0;
            enter(L_SETUP);
            setupNext();
        } else if (!strcmp(w, "SYSTEM")) {
            if (linkState == L_SETUP) setupNext();
        } else if (!strcmp(w, "MODELS")) {
            char cnt[8], cur[32];
            word(&p, cnt, sizeof cnt);
            word(&p, cur, sizeof cur);
            curModel = findModel(cur);
            ui_modelsChanged();
            if (linkState == L_SETUP) setupNext();
        } else if (!strcmp(w, "USE")) {
            char id[32];
            word(&p, id, sizeof id);
            curModel = findModel(id);
            ui_modelsChanged();                     /* (it also remembers the choice in the prefs) */
            if (linkState == L_SETUP) setupNext();
        } else if (!strcmp(w, "BYE")) {
            /* the relay hangs up next: NO CARRIER follows */
        }
        return;
    }
    if (!strcmp(w, "MODEL")) {
        if (nModels < MAXMODELS) {
            Model *m = &models[nModels++];
            word(&p, m->id, sizeof m->id);
            word(&p, m->tag, sizeof m->tag);
            strncpy(m->label, p, sizeof m->label - 1);
            m->label[sizeof m->label - 1] = 0;
        }
        return;
    }
    if (!strcmp(w, "BEGIN") && linkState == L_REPLY) {
        char id[32], tag[9];
        word(&p, id, sizeof id);
        word(&p, tag, sizeof tag);
        replyBytes = 0;
        replyStart = TickCount();
        if (!discard) ui_beginReply(tag);
        return;
    }
    if (!strcmp(w, "END") && linkState == L_REPLY) {
        char stop[12];
        word(&p, stop, sizeof stop);
        replyTicks = (long)(TickCount() - replyStart);
        if (!discard) ui_endReply(stop);
        discard = 0;
        enter(L_READY);
        return;
    }
    if (!strcmp(w, "NOTE")) {
        if (!discard) ui_note(p);
        return;
    }
    if (!strcmp(w, "ERR")) {
        char code[12];
        word(&p, code, sizeof code);
        if (!strcmp(code, "PIN")) {
            fail(E_PIN, "The relay didn't accept the PIN. Set it in Connection > Settings\311");
            return;
        }
        if (!strcmp(code, "BUSY") || (discard && linkState == L_REPLY)) return;
        ui_error(!strcmp(code, "MODEL") ? E_MODEL : E_RELAY, p);
        if (linkState == L_REPLY) {
            replyTicks = (long)(TickCount() - replyStart);
            ui_endReply("error");
            discard = 0;
            enter(L_READY);
        } else if (linkState == L_SETUP) setupNext();
        return;
    }
    if (!strcmp(w, "BYE")) {
        word(&p, w, sizeof w);
        if (!strcmp(w, "PIN")) { fail(E_PIN, "The relay hung up: wrong PIN. Set it in Connection > Settings\311"); return; }
        if (linkState != L_BYE) ui_error(E_RELAY, *p ? p : "The relay hung up.");
        return;
    }
    /* PONG and anything unknown: ignored */
}

static void handleLine(const char *s)
{
    int r = result(s);
    if (linkState >= L_GREET && linkState <= L_BYE) {
        if (r == R_NOCARRIER) {                     /* the TCP connection is gone */
            if (linkState == L_BYE) closeAll();
            else fail(E_LOST, "The connection to the relay was lost (the modem said NO CARRIER).");
            return;
        }
        relayLine(s);
        return;
    }
    if (r == R_ECHO) return;
    /* already online with a relay from an earlier session (the modem kept the connection)? */
    if (startsWith(s, "ASKAI ") || startsWith(s, "ERR ") || startsWith(s, "PONG")) {
        if (linkState == L_WAKE || linkState == L_INIT) { sendHello(); return; }
    }
    switch (linkState) {
    case L_WAKE:
        if (r == R_OK) sendInit();
        break;
    case L_ESC2:
        if (r == R_OK) { sendStr("ATH\r"); enter(L_ESC3); }
        break;
    case L_ESC3:
        if (r == R_OK) { sendStr("AT\r"); tries = 0; enter(L_WAKE); }
        break;
    case L_INIT:
        if (r == R_OK) {
            char d[64];
            p2c(prefs.dial, d, sizeof d);
            if (startsWith(d, "AT") || startsWith(d, "at")) sendStr(d);
            else { sendStr("ATDT"); sendStr(d); }
            sendStr("\r");
            enter(L_DIAL);
        } else if (r == R_ERROR) fail(E_INIT, "The modem rejected the init string (ERROR). Check it in Connection > Settings\311");
        break;
    case L_DIAL:
        if (r == R_CONNECT) enter(L_GREET);
        else if (r != R_OTHER && r != R_OK) dialFailed(r);
        break;
    case L_HESC2:
        if (r == R_OK) { sendStr("ATH\r"); enter(L_HESC3); }
        break;
    case L_HESC3:
        if (r == R_OK || r == R_NOCARRIER) closeAll();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ timeouts */
static void timeouts(void)
{
    unsigned long t = since();
    switch (linkState) {
    case L_WAKE:
        if (t > 90) {
            if (++tries < 3) { sendStr("AT\r"); stateTick = TickCount(); }
            else if (!escTried) { escTried = 1; enter(L_ESC1); }      /* maybe still online */
            else fail(E_NOMODEM, "The modem doesn't answer. Check that it is on, the cable, and that its speed "
                                 "matches Connection > Settings\311");
        }
        break;
    case L_ESC1: case L_HESC1:
        if (t > 72 && ser_writeIdle()) {        /* 1.2 s of silence, then the escape */
            sendStr("+++");
            enter(linkState == L_ESC1 ? L_ESC2 : L_HESC2);
        }
        break;
    case L_ESC2:
        if (t > 180) { sendStr("AT\r"); tries = 0; enter(L_WAKE); }
        break;
    case L_ESC3:
        if (t > 180) { sendStr("AT\r"); tries = 0; enter(L_WAKE); }
        break;
    case L_INIT:
        if (t > 240) fail(E_INIT, "The modem didn't answer the init string. Check it in Connection > Settings\311");
        break;
    case L_DIAL:
        if (t > 60L * 50) dialFailed(R_NONE);
        break;
    case L_GREET:
        if (t > 60L * 6) sendHello();           /* the greeting may have been missed: say HELLO anyway */
        break;
    case L_HELLO:
        if (t > 60L * 12) {
            static char msg[160];
            strcpy(msg, "Connected, but no Ask AI relay answered at ");
            strcat(msg, dialShown);
            strcat(msg, ".");
            sendLine("BYE", NULL, 0);
            fail(E_NORELAY, msg);
        }
        break;
    case L_SETUP:
        if (t > 60L * 15) enter(L_READY);
        break;
    case L_BYE:
        if (t > 180) {
            if (direct) closeAll();
            else enter(L_HESC1);
        }
        break;
    case L_HESC2: case L_HESC3:
        if (t > 180) closeAll();
        break;
    default:
        break;
    }
}

void link_poll(void)
{
    char buf[256];
    long n, i;
    if (linkState == L_OFF) return;
    if (linkState == L_TERM) {
        while ((n = ser_read(buf, sizeof buf)) > 0) {
            ui_rxBytes(n);
            ui_termText(buf, (short)n);
        }
        return;
    }
    while ((n = ser_read(buf, sizeof buf)) > 0) {
        ui_rxBytes(n);
        if (linkState == L_REPLY) replyBytes += n;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') {
                line[lineLen] = 0;
                if (lineLen) handleLine(line);
                lineLen = 0;
                if (linkState == L_OFF) return;
            } else if (c != '\n' && c != 0) {
                if (lineLen < (short)sizeof line - 1) line[lineLen++] = c;
            }
        }
    }
    timeouts();
}

/* ------------------------------------------------------------------ requests from the UI */
int link_ask(const char *text, short n)
{
    if (linkState != L_READY) return 0;
    if (n > 1000) n = 1000;
    sendLine("ASK", text, n);
    replyBytes = 0;
    replyStart = TickCount();
    discard = 0;
    enter(L_REPLY);
    return 1;
}

void link_stop(void)
{
    if (linkState == L_REPLY) sendLine("STOP", NULL, 0);
}

void link_newChat(void)
{
    if (linkState == L_REPLY) {
        discard = 1;                     /* the rest of this reply belongs to the old chat */
        sendLine("STOP", NULL, 0);
    }
    if (link_online()) sendLine("NEW", NULL, 0);
}

void link_use(short idx)
{
    if (idx < 0 || idx >= nModels || linkState != L_READY) return;
    sendLine("USE", models[idx].id, (short)strlen(models[idx].id));
}

const char *link_stateName(void)
{
    switch (linkState) {
    case L_OFF: return "not connected";
    case L_WAKE: case L_ESC1: case L_ESC2: case L_ESC3: return "waking the modem\311";
    case L_INIT: return "setting up the modem\311";
    case L_DIAL: return "dialling\311";
    case L_GREET: case L_HELLO: case L_SETUP: return "connecting\311";
    case L_READY: return "connected";
    case L_REPLY: return "receiving";
    case L_TERM: return "modem terminal";
    default: return "hanging up\311";
    }
}
