/* Orbit Duel: AppleTalk transport through the .MPP driver (Inside Macintosh II, "The AppleTalk
 * Manager", and IM: Networking): Control calls to refNum -10 with our own parameter blocks (the
 * Retro68 interfaces have no AppleTalk.h), a DDP socket whose listener (68000 assembly below)
 * copies each datagram into a small ring, and NBP to register "<Mac name>:Orbit Duel@*" and look up
 * the other players. Works with the Plus ROM's AppleTalk (version 19) and later versions.
 */
#include <Multiverse.h>
#include "atalk.h"

#define MPP_REF (-10)
enum { CS_WRITEDDP = 246, CS_CLOSESKT = 247, CS_OPENSKT = 248, CS_LOOKUP = 251, CS_REMOVE = 252,
       CS_REGISTER = 253, CS_SELFSEND = 256 };
#define DDP_TYPE 0x4F                       /* our DDP protocol type ('O'; 1-15 are Apple's) */

/* The MPP/ATP parameter block: the standard header up to csCode, then call parameters at 28. */
typedef struct MPB {
    void *qLink;                            /*  0 */
    short qType;                            /*  4 */
    short ioTrap;                           /*  6 */
    void *ioCmdAddr;                        /*  8 */
    void *ioCompletion;                     /* 12 */
    volatile short ioResult;                /* 16: 1 while an asynchronous call is in progress */
    long userData;                          /* 18 */
    short reqTID;                           /* 22 */
    short ioRefNum;                         /* 24 */
    short csCode;                           /* 26 */
    unsigned char p[52];                    /* 28.. */
} MPB;
typedef char mpb_layout_check[(sizeof(MPB) == 80) ? 1 : -1];
#define P8(pb, o) ((pb)->p[(o) - 28])
#define P16(pb, o) (*(short *)&(pb)->p[(o) - 28])
#define P32(pb, o) (*(long *)&(pb)->p[(o) - 28])

static void pb_clear(MPB *pb, short cs)
{
    short i;
    unsigned char *b = (unsigned char *)pb;
    for (i = 0; i < (short)sizeof *pb; i++)
        b[i] = 0;
    pb->ioRefNum = MPP_REF;
    pb->csCode = cs;
}

/* ---- the socket listener ---------------------------------------------------------------- */
#define RXN 8
#define RXMAX AT_MAX
typedef struct RxSlot {
    unsigned char node, skt, type, pad;
    short len;
    unsigned char data[RXMAX];
} RxSlot;                                   /* 86 bytes */
volatile unsigned char at_rx_head, at_rx_tail;
RxSlot at_rx_slots[RXN];
typedef char slot_check[(sizeof(RxSlot) == 86) ? 1 : -1];

/* Called by .MPP at interrupt level for each datagram to our socket (IM II-326): A2 = MPP's
   variables (the LAP header is at 1(A2)..3(A2)), A3 = just past the DDP header in the read-header
   area, A4 = ReadPacket (ReadRest at 2(A4)), D1 = bytes left. We may use D0, D2, D3 and A3 before
   ReadRest and must call ReadRest exactly once. Globals are addressed absolutely (Retro68
   relocates them at load time), so A5 isn't needed. */
__asm__(
    "    .text\n"
    "    .even\n"
    "    .globl  ddp_listener\n"
    "ddp_listener:\n"
    "    moveq   #0,%d2\n"
    "    move.b  -1(%a3),%d2\n"             /* DDP type */
    "    lsl.w   #8,%d2\n"
    "    move.b  -2(%a3),%d2\n"             /* source socket */
    "    moveq   #0,%d3\n"
    "    move.b  2(%a2),%d3\n"              /* LAP source node (short DDP header) */
    "    cmpi.b  #1,3(%a2)\n"
    "    beq.s   1f\n"
    "    move.b  -4(%a3),%d3\n"             /* long header: the DDP source node */
    "1:  move.b  at_rx_head,%d0\n"
    "    sub.b   at_rx_tail,%d0\n"
    "    cmpi.b  #8,%d0\n"
    "    bcc.s   8f\n"                      /* ring full: throw it away */
    "    moveq   #0,%d0\n"
    "    move.b  at_rx_head,%d0\n"
    "    andi.w  #7,%d0\n"
    "    mulu.w  #86,%d0\n"
    "    lea     at_rx_slots,%a3\n"
    "    adda.l  %d0,%a3\n"
    "    move.b  %d3,(%a3)\n"
    "    move.b  %d2,1(%a3)\n"
    "    lsr.w   #8,%d2\n"
    "    move.b  %d2,2(%a3)\n"
    "    addq.l  #6,%a3\n"
    "    moveq   #80,%d3\n"
    "    jsr     2(%a4)\n"                  /* ReadRest: Z set if the frame was good */
    "    bne.s   9f\n"
    "    tst.w   %d3\n"
    "    bmi.s   9f\n"                      /* longer than our buffer: don't keep it */
    "    moveq   #0,%d0\n"
    "    move.b  at_rx_head,%d0\n"
    "    andi.w  #7,%d0\n"
    "    mulu.w  #86,%d0\n"
    "    lea     at_rx_slots,%a3\n"
    "    adda.l  %d0,%a3\n"
    "    moveq   #80,%d0\n"
    "    sub.w   %d3,%d0\n"
    "    move.w  %d0,4(%a3)\n"
    "    addq.b  #1,at_rx_head\n"
    "9:  rts\n"
    "8:  moveq   #0,%d3\n"
    "    jmp     2(%a4)\n");
extern void ddp_listener(void);

/* ---- state ------------------------------------------------------------------------------ */
uint32_t at_my_addr;
short at_version;
AtPeer at_found[8];
short at_nfound, at_self_seen, at_total;
unsigned char at_my_name[34];
static short started, registered;
static unsigned char socket_no;
static MPB pb_reg, pb_look, pb_tx, pb_misc;
static long nte[28];                        /* names table entry, 108 bytes (+ spare), even address */
static unsigned char look_entity[40], my_entity[100];
static unsigned char look_buf[8 * 110];
typedef struct WdsEntry { short len; void *ptr; } WdsEntry;   /* 6 bytes on the 68000 */
static WdsEntry wds[3];                     /* write data structure: header, data, end */
static short hdr[9];                        /* 17-byte DDP header block (+1); H = hdr + 1 is odd */
static unsigned char tx_data[AT_MAX];

static short pcat(unsigned char *d, const unsigned char *s)   /* append Pascal string s at d */
{
    short i;
    for (i = 0; i <= s[0]; i++)
        d[i] = s[i];
    return s[0] + 1;
}

short at_state_off(void)
{
    return (*(volatile unsigned char *)0x1FB & 0x0F) > 1;
}

static void make_name(void)
{
    StringHandle h = GetString(-16096);     /* the Chooser's "Macintosh name" */
    SysEnvRec env;
    short i, n = 0;
    unsigned char node = (unsigned char)(at_my_addr >> 8);
    if (h && *h && (*h)[0] > 0) {
        n = (*h)[0] > 26 ? 26 : (*h)[0];
        for (i = 1; i <= n; i++)
            at_my_name[i] = (*h)[i];
    } else {
        const char *m;
        SysEnvirons(1, &env);
        m = env.machineType == 3 ? "Mac SE" : env.machineType == 2 ? "Mac Plus" : "Macintosh";   /* envSE, envMacPlus */
        for (i = 0; m[i]; i++)
            at_my_name[++n] = (unsigned char)m[i];
    }
    /* the node number makes names unique even when both Macs have the same name (or none) */
    at_my_name[++n] = ' ';
    at_my_name[++n] = '#';
    if (node >= 100) at_my_name[++n] = (unsigned char)('0' + node / 100);
    if (node >= 10) at_my_name[++n] = (unsigned char)('0' + node / 10 % 10);
    at_my_name[++n] = (unsigned char)('0' + node % 10);
    at_my_name[0] = (unsigned char)n;
}

short at_start(short turn_on)
{
    short ref, e;
    volatile unsigned char *spconfig = (volatile unsigned char *)0x1FB;
    volatile unsigned char *portbuse = (volatile unsigned char *)0x291;
    unsigned char *abus;
    SysEnvRec env;
    if (started)
        return 0;
    if (!(*portbuse & 0x80) && (*portbuse & 0x0F) != 1)
        return AT_ERR_INUSE;                /* port B belongs to another driver */
    if ((*spconfig & 0x0F) > 1) {
        if (!turn_on)
            return AT_ERR_OFF;
        /* what the Chooser's "AppleTalk Active" does: port B use = AppleTalk, saved in PRAM */
        *spconfig = (unsigned char)((*spconfig & 0xF0) | 1);
        WriteParam();
    }
    e = OpenDriver("\p.MPP", &ref);
    if (e)
        return e;
    SysEnvirons(1, &env);
    at_version = env.atDrvrVersNum;
    if (at_version == 0) {                  /* System 6.0 leaves it 0: the version byte of the DCE */
        Handle *ut = *(Handle **)0x11C;     /* UTableBase; .MPP is unit 9 */
        if (ut && ut[9] && *ut[9])
            at_version = ((unsigned char *)*ut[9])[7];
    }
    abus = *(unsigned char **)0x2D8;        /* ABusVars: sysLAPAddr at 0, sysNetNum at $1A */
    /* a DDP socket with our listener */
    pb_clear(&pb_misc, CS_OPENSKT);
    P8(&pb_misc, 28) = 0;                   /* any free socket */
    P32(&pb_misc, 30) = (long)ddp_listener;
    e = PBControlSync((ParmBlkPtr)&pb_misc);
    if (e)
        return e;
    socket_no = P8(&pb_misc, 28);
    at_rx_head = at_rx_tail = 0;
    at_my_addr = abus ? ((uint32_t)(unsigned short)*(short *)(abus + 0x1A) << 16) | ((uint32_t)abus[0] << 8) | socket_no
                      : socket_no;
    make_name();
    started = 1;
    return 0;
}

short at_is_started(void) { return started; }

short at_register_begin(void)
{
    unsigned char *e = (unsigned char *)nte + 9;
    short n = 0;
    if (!started)
        return -1;
    nte[0] = 0;
    ((unsigned char *)nte)[7] = socket_no;
    ((unsigned char *)nte)[8] = 0;
    n += pcat(e + n, at_my_name);
    n += pcat(e + n, AT_NBP_TYPE);
    n += pcat(e + n, "\p*");
    /* the same entity, packed, for RemoveName */
    for (n = 0; n < 99; n++)
        my_entity[n] = e[n];
    pb_clear(&pb_reg, CS_REGISTER);
    P8(&pb_reg, 28) = 2;                    /* retry interval: 2 x 8 ticks */
    P8(&pb_reg, 29) = 3;                    /* tries */
    P32(&pb_reg, 30) = (long)nte;
    P8(&pb_reg, 34) = 1;                    /* verify that nobody else has the name */
    return PBControlAsync((ParmBlkPtr)&pb_reg);
}

short at_poll_register(void)
{
    if (pb_reg.ioResult > 0)
        return 1;
    if (pb_reg.ioResult == 0)
        registered = 1;
    return pb_reg.ioResult;
}

short at_lookup_begin(void)
{
    short n = 0;
    if (!started || pb_look.ioResult > 0)
        return -1;
    n += pcat(look_entity + n, "\p=");
    n += pcat(look_entity + n, AT_NBP_TYPE);
    n += pcat(look_entity + n, "\p*");
    pb_clear(&pb_look, CS_LOOKUP);
    P8(&pb_look, 28) = 2;
    P8(&pb_look, 29) = 3;
    P32(&pb_look, 30) = (long)look_entity;
    P32(&pb_look, 34) = (long)look_buf;
    P16(&pb_look, 38) = (short)sizeof look_buf;
    P16(&pb_look, 40) = 8;                  /* at most 8 names */
    return PBControlAsync((ParmBlkPtr)&pb_look);
}

short at_poll_lookup(void)
{
    short i, got, n = 0, self = 0;
    unsigned char *t = look_buf;
    if (pb_look.ioResult > 0)
        return 1;
    if (pb_look.ioResult < 0 && pb_look.ioResult != -1024)   /* nbpBuffOvr still returns names */
        return pb_look.ioResult;
    got = P16(&pb_look, 42);
    for (i = 0; i < got && t < look_buf + sizeof look_buf - 8; i++) {
        uint32_t addr = ((uint32_t)(unsigned short)((t[0] << 8) | t[1]) << 16) | ((uint32_t)t[2] << 8) | t[3];
        unsigned char *name = t + 5;
        short k;
        if ((addr & 0xFFFF) == (at_my_addr & 0xFFFF))
            self = 1;                       /* our own name (node and socket match) */
        else if (n < 8) {
            at_found[n].addr = addr;
            for (k = 0; k <= name[0] && k < 33; k++)
                at_found[n].name[k] = name[k];
            if (name[0] > 32)
                at_found[n].name[0] = 32;
            n++;
        }
        t = name + name[0] + 1;             /* object, then type, then zone */
        t += t[0] + 1;
        t += t[0] + 1;
    }
    at_nfound = n;
    at_self_seen = self;
    at_total = got;
    return 0;
}

short at_send(uint32_t addr, const unsigned char *p, short n)
{
    unsigned char *b = (unsigned char *)hdr;
    short i;
    if (!started || n > AT_MAX)
        return -1;
    if (pb_tx.ioResult > 0)
        return -1;                          /* the previous datagram is still going */
    for (i = 0; i < n; i++)
        tx_data[i] = p[i];
    /* header block H = b + 1: destination net at H+7, node H+11, socket H+13, DDP type H+15 */
    *(short *)(b + 8) = (short)(addr >> 16);
    b[12] = (unsigned char)(addr >> 8);
    b[14] = (unsigned char)addr;
    b[16] = DDP_TYPE;
    wds[0].len = 0;
    wds[0].ptr = b + 1;
    wds[1].len = n;
    wds[1].ptr = tx_data;
    wds[2].len = 0;
    pb_clear(&pb_tx, CS_WRITEDDP);
    P8(&pb_tx, 28) = socket_no;
    P8(&pb_tx, 29) = 0;                     /* no DDP checksum (LocalTalk frames have a CRC) */
    P32(&pb_tx, 30) = (long)wds;
    return PBControlAsync((ParmBlkPtr)&pb_tx);
}

short at_recv(unsigned char *p, short max, uint32_t *from)
{
    RxSlot *s;
    short i, n;
    if (at_rx_head == at_rx_tail)
        return 0;
    s = &at_rx_slots[at_rx_tail & 7];
    n = s->len > max ? max : s->len;
    for (i = 0; i < n; i++)
        p[i] = s->data[i];
    /* short DDP headers carry no network number: the sender is on our own network */
    *from = (at_my_addr & 0xFFFF0000UL) | ((uint32_t)s->node << 8) | s->skt;
    at_rx_tail++;
    return n ? n : -1;
}

short at_set_self_send(short on)
{
    short e;
    if (at_version < 48)
        return -1;
    pb_clear(&pb_misc, CS_SELFSEND);
    P8(&pb_misc, 28) = (unsigned char)(on ? 1 : 0);
    e = PBControlSync((ParmBlkPtr)&pb_misc);
    return e ? e : P8(&pb_misc, 29);
}

void at_stop(void)
{
    unsigned long t0 = *(volatile unsigned long *)0x16A;
    if (!started)
        return;
    /* NBP and DDP keep pointers into our globals while a call is pending: let them finish */
    while ((pb_reg.ioResult > 0 || pb_look.ioResult > 0 || pb_tx.ioResult > 0)
           && *(volatile unsigned long *)0x16A - t0 < 300)
        ;
    if (pb_reg.ioResult == 0 || registered) {
        pb_clear(&pb_misc, CS_REMOVE);
        P32(&pb_misc, 30) = (long)my_entity;
        PBControlSync((ParmBlkPtr)&pb_misc);
        registered = 0;
    }
    pb_clear(&pb_misc, CS_CLOSESKT);
    P8(&pb_misc, 28) = socket_no;
    PBControlSync((ParmBlkPtr)&pb_misc);
    started = 0;                            /* .MPP stays open (Inside Macintosh: never close it) */
}
