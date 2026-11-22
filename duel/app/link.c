/* Orbit Duel: games between two Macs, over AppleTalk (atalk.c) or a serial cable (serial.c).
 *
 * Lockstep: both Macs run the same simulation (sim.c) from the same seed and exchange only their
 * players' inputs. The input this Mac samples while it is about to simulate step t is used at step
 * t + D (D = 3 steps, 100 ms; 4 at the slow MAME test speed), so it has D steps to reach the other
 * Mac. A step runs only when both inputs for it are known. Every packet carries all of this Mac's
 * inputs the other hasn't acknowledged yet, so a lost packet costs nothing but a resend, and quiet
 * links are kept alive by resending every 8 ticks. Every 30 steps (1 s) each Mac hashes its whole
 * state and sends the hash; the other compares it with its own for the same step (desync check).
 *
 * Starting: on a serial cable both Macs send HELLO with a random number and the higher one hosts.
 * On AppleTalk one player picks the other from the list (NBP lookup); the inviter hosts. The host
 * flies the Needle, picks the seed and the session number and sends START until the guest's first
 * inputs arrive.
 *
 * Packets (all integers big-endian):
 *   HELLO  'H' version nonce[4] name[pstr]          serial: looking for the other Mac
 *   INVITE 'V' version nonce[4] name[pstr]          AppleTalk: "play with me"
 *   ACCEPT 'A' version nonce[4]
 *   START  'S' version session seed[4] first_to delay
 *   INPUT  'I' session first[4] n inputs[n] ack[4] sum_tick[4] sum[4]
 *   BYE    'B' session                               leaving the game
 */
#include <Quickdraw.h>
#include <Events.h>
#include <string.h>
#include "sim.h"
#include "render.h"
#include "sound.h"
#include "main.h"
#include "link.h"
#include "serlink.h"
#include "atalk.h"

#define TICKS (*(volatile unsigned long *)(long)ticks_addr)
static volatile long ticks_addr = 0x16A;

#define VERSION 1
#define RING 64
#define FIRST_TO 5

enum { LS_OFF, LS_ASK_ON, LS_ERROR, LS_STARTING, LS_REGISTER, LS_LOOKING, LS_INVITING, LS_WAIT_START,
       LS_PLAYING, LS_ENDED };

static short lkind, state, role = -1, test_speed;
static unsigned long my_nonce;
static uint32_t peer_addr;
static unsigned char peer_name[34];
static unsigned char session;
static short delay_steps = 3, send_every = 1;
static unsigned long state_t, last_tx, last_rx, last_lookup, lookup_done;
static short err_code, sel, gframe, got_peer_input;
static const unsigned char *err_text;

/* lockstep */
static unsigned char local_in[RING], remote_in[RING];
static long local_next, remote_next, peer_ack, sent_upto;
static long cs_tick_l[8], cs_tick_r[8], compared_upto;
static uint32_t cs_l[8], cs_r[8], last_sum;
static long last_sum_tick;
static short desync, ended_why;

static unsigned char pk[96];

/* ---- helpers ------------------------------------------------------------------------------ */
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}
static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void set_state(short s)
{
    state = s;
    state_t = TICKS;
    dbg.link_state = s;
}

static unsigned long rnd32(void)
{
    static unsigned long r;
    Point m;
    GetMouse(&m);
    r = r * 1103515245UL + 12345UL + TICKS + ((unsigned long)m.h << 16) + (unsigned long)m.v;
    return r ^ (r >> 15);
}

static void tx(const unsigned char *p, short n)
{
    short e = lkind == LINK_SERIAL ? ser_send(p, n) : at_send(peer_addr, p, n);
    if (e == 0) {
        dbg.pk_tx++;
        last_tx = TICKS;
    }
}

static short rx(unsigned char *p, short max, uint32_t *from)
{
    short n;
    *from = 0;
    n = lkind == LINK_SERIAL ? ser_recv(p, max) : at_recv(p, max, from);
    if (n > 0) {
        dbg.pk_rx++;
        last_rx = TICKS;
    }
    dbg.pk_bad = ser_bad;
    return n;
}

static void send_hello(char type)
{
    short n = 0;
    pk[n++] = (unsigned char)type;
    pk[n++] = VERSION;
    put32(pk + n, my_nonce);
    n += 4;
    if (type != 'A') {
        short k;
        const unsigned char *nm = lkind == LINK_ATALK ? (const unsigned char *)at_my_name : (const unsigned char *)"\pMac";
        for (k = 0; k <= nm[0]; k++)
            pk[n++] = nm[k];
    }
    tx(pk, n);
}

static void send_start(void)
{
    pk[0] = 'S';
    pk[1] = VERSION;
    pk[2] = session;
    put32(pk + 3, (uint32_t)dbg.seed);
    pk[7] = FIRST_TO;
    pk[8] = (unsigned char)delay_steps;
    tx(pk, 9);
}

static void send_bye(void)
{
    pk[0] = 'B';
    pk[1] = session;
    tx(pk, 2);
}

static void send_input(void)
{
    long first = peer_ack + 1, k;
    short n, i = 0;
    if (first < local_next - 16)
        first = local_next - 16;
    n = (short)(local_next - first);
    if (n < 0)
        n = 0;
    pk[i++] = 'I';
    pk[i++] = session;
    put32(pk + i, (uint32_t)first);
    i += 4;
    pk[i++] = (unsigned char)n;
    for (k = 0; k < n; k++)
        pk[i++] = local_in[(first + k) & (RING - 1)];
    put32(pk + i, (uint32_t)(remote_next - 1));
    i += 4;
    put32(pk + i, (uint32_t)last_sum_tick);
    i += 4;
    put32(pk + i, last_sum);
    i += 4;
    tx(pk, i);
    sent_upto = local_next;
}

/* ---- checksums ------------------------------------------------------------------------------ */
static void compare_sums(void)
{
    short k;
    for (k = 0; k < 8; k++) {
        long t = cs_tick_r[k];
        short j = (short)((t / 30) & 7);
        if (t > compared_upto && t > 0 && cs_tick_l[j] == t) {
            if (cs_l[j] == cs_r[k]) dbg.csum_ok++;
            else { dbg.csum_bad++; desync = 1; }
            dbg.last_csum = (int32_t)cs_l[j];
            compared_upto = t;
        }
    }
}

static void local_sum(const Sim *s)
{
    short j = (short)((s->tick / 30) & 7);
    last_sum = sim_checksum(s);
    last_sum_tick = s->tick;
    cs_tick_l[j] = s->tick;
    cs_l[j] = last_sum;
    if (s->tick / 30 - 1 < 8)
        dbg.sum_hist[s->tick / 30 - 1] = (int32_t)last_sum;
    compare_sums();
}

/* ---- starting a game ---------------------------------------------------------------------- */
static void begin_game(unsigned long seed, short ship, unsigned char sess, short delay)
{
    short i;
    role = ship;
    session = sess;
    delay_steps = delay;
    send_every = test_speed ? 2 : 1;
    for (i = 0; i < RING; i++)
        local_in[i] = remote_in[i] = 0;
    local_next = remote_next = delay;           /* steps 0 .. delay-1 have no input on either side */
    peer_ack = delay - 1;
    sent_upto = 0;
    for (i = 0; i < 8; i++) {
        cs_tick_l[i] = cs_tick_r[i] = -1;
        dbg.sum_hist[i] = 0;
    }
    compared_upto = 0;
    last_sum = 0;
    last_sum_tick = 0;
    desync = 0;
    got_peer_input = 0;
    dbg.role = ship;
    dbg.seed = (int32_t)seed;
    dbg.delay = delay;
    dbg.csum_ok = dbg.csum_bad = 0;
    dbg.waits = 0;
    last_rx = TICKS;
    set_state(LS_PLAYING);
    sound_event(SND_LINK);
    start_net_game(seed, ship);
}

static void host_game(void)
{
    unsigned long seed = rnd32();
    session = (unsigned char)(1 + rnd32() % 255);
    dbg.seed = (int32_t)seed;
    delay_steps = test_speed ? 4 : 3;
    begin_game(seed, 0, session, delay_steps);
    send_start();
}

/* ---- incoming packets --------------------------------------------------------------------- */
static void on_input(const unsigned char *p, short n)
{
    long first, ack, t, k;
    short cnt;
    if (n < 7 || p[1] != session)
        return;
    first = (long)get32(p + 2);
    cnt = p[6];
    if (n < 7 + cnt + 12)
        return;
    got_peer_input = 1;
    if (first <= remote_next) {
        for (k = 0; k < cnt; k++) {
            t = first + k;
            if (t >= remote_next)
                remote_in[t & (RING - 1)] = p[7 + k];
        }
        if (first + cnt > remote_next)
            remote_next = first + cnt;
    }
    ack = (long)get32(p + 7 + cnt);
    if (ack > peer_ack)
        peer_ack = ack;
    t = (long)get32(p + 11 + cnt);
    if (t > 0) {
        short j = (short)((t / 30) & 7);
        cs_tick_r[j] = t;
        cs_r[j] = get32(p + 15 + cnt);
        compare_sums();
    }
    dbg.remote_tick = remote_next;
}

static void end_link(short why)
{
    ended_why = why;
    set_state(LS_ENDED);
}

static void handle_packets(void)
{
    uint32_t from;
    short n, guard = 0;
    while (guard++ < 12 && (n = rx(pk, sizeof pk, &from)) != 0) {
        unsigned char type;
        if (n < 0)
            continue;
        type = pk[0];
        if (lkind == LINK_ATALK && state >= LS_WAIT_START && from != peer_addr && type != 'V' && type != 'P')
            continue;                               /* not our opponent */
        if ((type == 'H' || type == 'V' || type == 'A' || type == 'S') && pk[1] != VERSION) {
            err_text = "\pThe other Mac has a different version of Orbit Duel.";
            set_state(LS_ERROR);
            return;
        }
        switch (type) {
        case 'H':                                   /* serial: the other Mac is there */
            if (state == LS_LOOKING || state == LS_WAIT_START) {
                unsigned long theirs = get32(pk + 2);
                if (theirs == my_nonce)
                    my_nonce = rnd32();             /* a tie: try again */
                else if (my_nonce > theirs)
                    host_game();
                else
                    set_state(LS_WAIT_START);
            } else if (state == LS_PLAYING && TICKS - state_t > 120)
                end_link(1);                        /* it went back to looking: it left */
            break;
        case 'V':                                   /* AppleTalk: an invitation */
            if (state == LS_LOOKING || state == LS_INVITING) {
                short k;
                if (state == LS_INVITING && from == peer_addr && at_my_addr < from)
                    break;                          /* we invited each other: the lower address hosts */
                peer_addr = from;
                for (k = 0; k <= pk[6] && k < 33 && 6 + k < n; k++)
                    peer_name[k] = pk[6 + k];
                set_state(LS_WAIT_START);
                send_hello('A');
            }
            break;
        case 'A':
            if (state == LS_INVITING && from == peer_addr)
                host_game();
            break;
        case 'S':
            if (state == LS_LOOKING || state == LS_WAIT_START || state == LS_INVITING
                || (state == LS_PLAYING && role == 1 && pk[2] != session)) {
                if (lkind == LINK_ATALK)
                    peer_addr = from;
                begin_game(get32(pk + 3), 1, pk[2], pk[8]);
            }
            break;
        case 'I':
            if (state == LS_PLAYING)
                on_input(pk, n);
            break;
        case 'B':
            if (state == LS_PLAYING && pk[1] == session)
                end_link(2);
            break;
        case 'P':                                   /* self-test: a datagram we sent to ourselves */
            if (lkind == LINK_ATALK && from == at_my_addr) {
                dbg.ddp_loop++;
                dbg.self_test |= 32;
            }
            break;
        }
    }
}

/* ---- the game (called from main.c's game_frame) ---------------------------------------------- */
static void sample_local(const Sim *s)
{
    while (local_next <= s->tick + delay_steps && local_next - peer_ack < RING - 4) {
        local_in[local_next & (RING - 1)] = local_input();
        local_next++;
    }
}

short link_game_steps(short *events)
{
    Sim *s = game_sim();
    short n = 0, due;
    handle_packets();
    if (state == LS_ENDED) {
        hud_note_set(30 + ended_why, ended_why == 2 ? "\pThe other player left.  Press Return."
                                                    : "\pThe other Mac stopped answering.  Press Return.");
        if (key_hit(0x24) || key_hit(0x31))
            back_to_title();
        return 0;
    }
    if (state != LS_PLAYING)
        return 0;
    due = steps_due(s->tick);
    sample_local(s);
    while (n < due && remote_next > s->tick) {
        long t = s->tick;
        unsigned char a = local_in[t & (RING - 1)], b = remote_in[t & (RING - 1)];
        if (role == 0)
            sim_step(s, a, b);
        else
            sim_step(s, b, a);
        *events |= s->events;
        n++;
        if (s->tick % 30 == 0)
            local_sum(s);
        sample_local(s);
    }
    if (role == 0 && !got_peer_input && TICKS - last_tx >= 15)
        send_start();
    else if (local_next - sent_upto >= send_every || TICKS - last_tx >= 8)
        send_input();
    if (due > 0 && n == 0) {
        /* waiting for the other Mac: move our schedule back so that, when its input arrives, we take
           one step per frame instead of catching up in bursts. The Mac that is ahead falls into step
           with the other one this way (otherwise a host that started a moment early would draw only
           every other step for the whole game). */
        dbg.waits++;
        pace_reset(s->tick);
    }
    if (desync)
        hud_note_set(20, "\pOUT OF SYNC: the two Macs disagree about the game.");
    else if (TICKS - last_rx > 180)
        hud_note_set(21, "\pWaiting for the other Mac...");
    else
        hud_note_set(0, 0);
    if (TICKS - last_rx > 60L * 30)
        end_link(1);                                /* nothing for 30 s */
    return n;
}

void link_idle(void)
{
    sound_tick();
}

void link_leave(void)
{
    short i;
    unsigned long t;
    if (state == LS_PLAYING)
        for (i = 0; i < 3; i++) {                   /* three tries; no answer needed */
            send_bye();
            for (t = TICKS; TICKS - t < 6;)
                ;
        }
    set_state(LS_OFF);
}

/* ---- the link screens ------------------------------------------------------------------------ */
#define LIST_Y 52
#define LIST_ROW 16
#define LIST_N 6
#define TEXT_Y 206

static void center(const unsigned char *s, short y)
{
    MoveTo((ARENA_W - StringWidth(s)) / 2, y);
    DrawString(s);
}

static void draw_screen(void)
{
    short i;
    r_stars(0x11A7);
    r_bg_black(24, 6, ARENA_W - 24, 46);
    r_text_c(lkind == LINK_ATALK ? "APPLETALK GAME" : "SERIAL CABLE GAME", 16, 3, 0);
    r_bg_black(24, TEXT_Y - 16, ARENA_W - 24, ARENA_H - 4);
    if (lkind == LINK_ATALK && (state == LS_LOOKING || state == LS_INVITING || state == LS_WAIT_START))
        r_bg_black(64, LIST_Y - 4, ARENA_W - 64, LIST_Y + LIST_N * LIST_ROW + 4);
    r_qd_begin();
    if (lkind == LINK_ATALK && (state == LS_LOOKING || state == LS_INVITING || state == LS_WAIT_START)) {
        if (at_nfound == 0) {
            center("\pLooking for other Macs running Orbit Duel...", LIST_Y + 2 * LIST_ROW);
        } else
            for (i = 0; i < at_nfound && i < LIST_N; i++) {
                MoveTo(84, LIST_Y + 12 + i * LIST_ROW);
                DrawString(at_found[i].name);
            }
    }
    switch (state) {
    case LS_ASK_ON:
        center("\pAppleTalk is turned off on this Mac.", TEXT_Y);
        center("\pOrbit Duel can turn it on, as the Chooser's \"Active\" button does.", TEXT_Y + 18);
        center("\pOnly do this if the printer port goes to the other Mac", TEXT_Y + 36);
        center("\p(LocalTalk or PhoneNet boxes, or a printer cable), not to a printer.", TEXT_Y + 54);
        center("\pReturn: turn AppleTalk on        Esc: back", TEXT_Y + 84);
        break;
    case LS_STARTING:
        center(lkind == LINK_ATALK ? "\pStarting AppleTalk..." : "\pOpening the modem port...", TEXT_Y + 30);
        break;
    case LS_REGISTER:
        center("\pJoining the network...", TEXT_Y + 30);
        break;
    case LS_ERROR:
        if (err_text)
            center(err_text, TEXT_Y + 8);
        if (lkind == LINK_ATALK) {
            center("\pCheck: Apple menu > Chooser > AppleTalk Active (then restart),", TEXT_Y + 30);
            center("\pand that the printer ports are joined, not used by a printer.", TEXT_Y + 48);
        } else {
            center("\pIs the modem port in use by another program?", TEXT_Y + 30);
        }
        {
            Str255 s;
            NumToString(err_code, s);
            MoveTo(ARENA_W / 2 - 40, TEXT_Y + 72);
            DrawString("\pError code ");
            DrawString(s);
        }
        center("\pEsc: back", TEXT_Y + 100);
        break;
    case LS_LOOKING:
        if (lkind == LINK_ATALK) {
            center("\pYou are on the network as:", TEXT_Y);
            center(at_my_name, TEXT_Y + 18);
            center(at_nfound ? "\pPick an opponent: W / S or click, then Return."
                             : "\pStart Orbit Duel's AppleTalk game on the other Mac too.", TEXT_Y + 46);
            center("\pEsc: back", TEXT_Y + 100);
        } else {
            center("\pLooking for the other Mac on the modem port...", TEXT_Y);
            center("\pJoin the two modem ports with a Mac null-modem cable", TEXT_Y + 28);
            center("\p(mini-DIN-8 to mini-DIN-8, \"Mac to Mac\" or \"Mac printer\" cable),", TEXT_Y + 46);
            center("\pand choose Serial Cable Game on both Macs.", TEXT_Y + 64);
            center("\pEsc: back", TEXT_Y + 100);
        }
        break;
    case LS_INVITING: {
        center("\pAsking", TEXT_Y + 10);
        center(peer_name, TEXT_Y + 28);
        center("\pEsc: back", TEXT_Y + 100);
        break;
    }
    case LS_WAIT_START:
        center("\pFound the other Mac. Starting...", TEXT_Y + 20);
        if (lkind == LINK_ATALK)
            center(peer_name, TEXT_Y + 38);
        break;
    }
    r_qd_end();
    if (lkind == LINK_ATALK && state == LS_LOOKING && at_nfound > 0 && sel < at_nfound)
        r_bg_invert(80, LIST_Y + sel * LIST_ROW, ARENA_W - 80, LIST_Y + (sel + 1) * LIST_ROW);
    full_redraw();
}

static void fail(short e, const unsigned char *text)
{
    err_code = e;
    err_text = text;
    dbg.err = e;
    set_state(LS_ERROR);
}

static void atalk_begin(short turn_on)
{
    short e;
    set_state(LS_STARTING);
    draw_screen();
    e = at_start(turn_on);
    dbg.mpp_open = e;
    dbg.at_version = at_version;
    if (e == AT_ERR_OFF) {
        set_state(LS_ASK_ON);
        return;
    }
    if (e == AT_ERR_INUSE) {
        fail(e, "\pThe printer port is in use by another driver.");
        return;
    }
    if (e) {
        fail(e, "\pAppleTalk could not start.");
        return;
    }
    dbg.self_test |= 1;
    dbg.node = (at_my_addr >> 8) & 0xFF;
    dbg.net = at_my_addr >> 16;
    dbg.socket = at_my_addr & 0xFF;
    dbg.self_test |= 2;
    e = at_register_begin();
    if (e) {
        fail(e, "\pCould not register on the network.");
        return;
    }
    set_state(LS_REGISTER);
}

void link_screen(short which)
{
    lkind = which;
    dbg.link_kind = which;
    test_speed = (short)(dbg.arg == 77);            /* the MAME test speed (serial.c) */
    sel = 0;
    role = -1;
    dbg.role = -1;
    peer_addr = 0;
    my_nonce = rnd32();
    at_nfound = 0;
    set_screen_link();
    r_set_top_limit(0);
    if (which == LINK_SERIAL) {
        short e = ser_open(test_speed);
        dbg.serial_open = e;
        if (e)
            fail(e, "\pThe modem port could not be opened.");
        else
            set_state(LS_LOOKING);
    } else {
        dbg.spconfig = (*(volatile unsigned char *)0x1FB << 8) | *(volatile unsigned char *)0x291;
        if (at_state_off())
            set_state(LS_ASK_ON);
        else
            atalk_begin(0);
    }
    draw_screen();
}

void link_close(void)
{
    if (ser_is_open())
        ser_close();
    if (at_is_started())
        at_stop();
    lkind = 0;
    dbg.link_kind = 0;
    set_state(LS_OFF);
}

void link_init(void)
{
    state = LS_OFF;
}

static void invite(short i)
{
    short k;
    if (i < 0 || i >= at_nfound)
        return;
    peer_addr = at_found[i].addr;
    for (k = 0; k <= at_found[i].name[0]; k++)
        peer_name[k] = at_found[i].name[k];
    set_state(LS_INVITING);
    send_hello('V');
    draw_screen();
}

void link_frame(void)
{
    Point click = {0, 0};
    short key, old_state = state, redraw = 0;
    keys_poll();
    key = poll_events(&click);
    if (state == LS_OFF)
        return;
    if (esc_pressed() || key == 0x1B || dbg.cmd == CMD_TITLE) {
        dbg.cmd = 0;
        back_to_title();
        return;
    }
    if (ui_take_dirty())
        redraw = 1;
    handle_packets();
    if (state == LS_PLAYING)
        return;                                     /* a game started */
    switch (state) {
    case LS_ASK_ON:
        if (key == '\r' || key == 3 || dbg.cmd == CMD_ATALK) {
            dbg.cmd = 0;
            atalk_begin(1);
            redraw = 1;
        }
        break;
    case LS_REGISTER: {
        short r = at_poll_register();
        dbg.nbp_reg = r;
        if (r == 0) {
            dbg.self_test |= 4;
            if (dbg.arg == 78 && at_set_self_send(1) >= 0) {     /* test: loop a datagram back to us */
                unsigned char ping[2] = {'P', 0};
                dbg.self_test |= 16;
                at_send(at_my_addr, ping, 2);
            }
            set_state(LS_LOOKING);
            if (at_lookup_begin() == 0) {
                last_lookup = TICKS;
                dbg.nbp_lookups++;
            }
        } else if (r < 0)
            fail(r, r == -1027 ? "\pAnother Mac already uses this name." : "\pCould not register on the network.");
        break;
    }
    case LS_LOOKING:
    case LS_INVITING:
    case LS_WAIT_START:
        if (lkind == LINK_SERIAL) {
            if (TICKS - last_tx >= 30)
                send_hello('H');
            break;
        }
        if (last_lookup && at_poll_lookup() == 0) {
            short before = at_nfound;
            dbg.nbp_found = at_total;
            dbg.opponents = at_nfound;
            if (at_self_seen)
                dbg.self_test |= 8;
            if (sel >= at_nfound)
                sel = at_nfound ? at_nfound - 1 : 0;
            if (state == LS_LOOKING && (before != at_nfound || at_nfound))
                redraw = 1;
            last_lookup = 0;
            lookup_done = TICKS;
        }
        if (!last_lookup && TICKS - lookup_done >= 60 && at_lookup_begin() == 0) {
            last_lookup = TICKS;
            dbg.nbp_lookups++;
        }
        if (state == LS_LOOKING) {
            if ((key == 'w' || key == 'W' || key == 0x1E) && sel > 0) { sel--; redraw = 1; }
            if ((key == 's' || key == 'S' || key == 0x1F) && sel < at_nfound - 1) { sel++; redraw = 1; }
            if (key == '\r' || key == ' ' || key == 3)
                invite(sel);
            if (dbg.cmd == CMD_INVITE) {
                dbg.cmd = 0;
                invite((short)dbg.arg);
            }
            if (click.v || click.h) {
                short y = click.v - r_view_top - LIST_Y;
                if (y >= 0 && y < LIST_N * LIST_ROW && y / LIST_ROW < at_nfound)
                    invite(y / LIST_ROW);
            }
        } else if (state == LS_INVITING) {
            if (TICKS - last_tx >= 20)
                send_hello('V');
            if (TICKS - state_t > 60L * 10) {       /* no answer in 10 s */
                set_state(LS_LOOKING);
                redraw = 1;
            }
        } else if (state == LS_WAIT_START && TICKS - last_tx >= 20)
            send_hello('A');
        break;
    default:
        break;
    }
    if (state == LS_PLAYING)
        return;
    if (state != old_state || redraw)
        draw_screen();
    present_glow(gframe++);
    sound_tick();
}
