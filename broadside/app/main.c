/*
 * Broadside: sink the Mac's fleet before it sinks yours. A Salvo-style ship game for the Mac
 * Plus (1 MB, System 6) and SE. This file: Toolbox setup, menus, the event loop, the flow of a
 * game, the Mac's messages, the About and Statistics windows, and the test mailbox.
 */
#include "broadside.h"

Game g;
WindowPtr gWin;
struct DebugBlock dbg;

static MenuHandle mApple, mFile, mOpts;
enum { M_APPLE = 128, M_FILE, M_OPTS };
enum { F_NEW = 1, F_STATS, F_SEP, F_QUIT };
enum { O_EASY = 1, O_NORMAL, O_HARD, O_SEP1, O_SALVO, O_NOTOUCH, O_SEP2, O_THINK, O_SOUND, O_SPEECH };

static short gAiLive;                 /* the AI's analysis is for the current knowledge */
static unsigned long gHeatNext;
static uint16_t gHeatSamples;
static void heat_draw(int n);

/* ---- the Mac's messages --------------------------------------------------------------------- */

static const char *pick(const char *const *list, int n)
{
    return list[bs_below(&g.rng, (uint16_t)n)];
}
#define PICK(list) pick(list, (int)(sizeof list / sizeof list[0]))

static const char *const kMacHit[] = {"Ha! A hit!", "Got one!", "Direct hit!", "I can see smoke!",
                                      "Bullseye!", "Found you!"};
static const char *const kMacMiss[] = {"Hmm. Just water.", "Splash. I'll get you yet.", "Nothing there?",
                                       "Missed. Recalculating...", "The sea is big."};
static const char *const kMyHit[] = {"Ouch!", "Lucky shot.", "Hey! That hurt.", "Hmm... you're good.",
                                     "My poor paint!"};
static const char *const kMyMiss[] = {"Missed me!", "Nothing but water.", "Ha! Try again.",
                                      "Just fish down there.", "Cold. Very cold."};

static void str_cat(char *d, const char *s)
{
    strcat(d, s);
}

/* ---- menus ---------------------------------------------------------------------------------- */

static void update_menus(void)
{
    CheckItem(mOpts, O_EASY, gPrefs.level == AI_EASY);
    CheckItem(mOpts, O_NORMAL, gPrefs.level == AI_NORMAL);
    CheckItem(mOpts, O_HARD, gPrefs.level == AI_HARD);
    CheckItem(mOpts, O_SALVO, gPrefs.salvo);
    CheckItem(mOpts, O_NOTOUCH, gPrefs.notouch);
    CheckItem(mOpts, O_THINK, gPrefs.think);
    CheckItem(mOpts, O_SOUND, gPrefs.sound);
    if (speech_available()) {
        EnableItem(mOpts, O_SPEECH);
        CheckItem(mOpts, O_SPEECH, gPrefs.speech);
    } else {
        CheckItem(mOpts, O_SPEECH, 0);
        DisableItem(mOpts, O_SPEECH);
    }
}

static void setup_menus(void)
{
    mApple = NewMenu(M_APPLE, "\p\x14");
    AppendMenu(mApple, "\pAbout Broadside\xC9;(-");
    AppendResMenu(mApple, 'DRVR');
    InsertMenu(mApple, 0);
    mFile = NewMenu(M_FILE, "\pFile");
    AppendMenu(mFile, "\pNew Game/N;Statistics\xC9;(-;Quit/Q");
    InsertMenu(mFile, 0);
    mOpts = NewMenu(M_OPTS, "\pOptions");
    AppendMenu(mOpts, "\pEasy/1;Normal/2;Hard/3;(-;Salvo Rules;No Touching;(-;Show the Mac\xD5s Thinking/T;"
                      "Sound/M;Speech");
    InsertMenu(mOpts, 0);
    DrawMenuBar();
    update_menus();
}

/* ---- waiting without starting anything new -------------------------------------------------- */

void quit_app(void)
{
    snd_quit();
    speech_quit();
    ExitToShell();
}

static int is_esc(const EventRecord *ev)
{
    short code = (short)((ev->message & keyCodeMask) >> 8);
    char ch = (char)(ev->message & charCodeMask);
    return ch == 0x1B || code == 0x32 || code == 0x35;
}

static void toggle_sound(void)
{
    gPrefs.sound ^= 1;
    update_menus();
    prefs_save();
}

void pump(short ticks)
{
    unsigned long until = TICKS + (unsigned long)ticks;
    EventRecord ev;
    dbg.state = (uint32_t)g.state + 1;
    do {
        SystemTask();
        if (GetNextEvent(updateMask | keyDownMask, &ev)) {
            if (ev.what == updateEvt) {
                WindowPtr w = (WindowPtr)ev.message;
                BeginUpdate(w);
                if (w == gWin)
                    draw_all();
                EndUpdate(w);
            } else if (ev.what == keyDown) {
                char ch = (char)(ev.message & charCodeMask);
                if (is_esc(&ev) || ((ev.modifiers & cmdKey) && (ch == 'q' || ch == 'Q')))
                    quit_app();
                if (ch == 'm' || ch == 'M')
                    toggle_sound();
            }
        }
    } while (TICKS < until);
}

/* ---- the heat map ("Show the Mac's Thinking") ----------------------------------------------- */

static short heat_level(uint8_t h)
{
    return (short)(h ? 1 + (h * 11 + 127) / 255 : 0);
}

static uint8_t gHeatDirty[NCELL];
static short gDirtyCount;

/* new heat map; cells whose shade changed are redrawn a few at a time from idle() (all at once
   when force) */
static void refresh_heat(int force)
{
    uint8_t h[NCELL];
    int i, old = gHeatTarget;
    if (!gPrefs.think || g.state == ST_PLACE)
        return;
    if (gAiLive && g.ai.done) {
        ai_heat(&g.ai, &g.know[ME], h);
        gHeatTarget = (short)ai_best(&g.ai, &g.know[ME]);
    } else {
        memset(h, 0, sizeof h);
        gHeatTarget = -1;
    }
    for (i = 0; i < NCELL; i++) {
        if (heat_level(h[i]) != heat_level(gHeat[i]) || force) {
            if (!gHeatDirty[i])
                gDirtyCount++;
            gHeatDirty[i] = 1;
        }
        gHeat[i] = h[i];
    }
    if (old != gHeatTarget) {
        if (old >= 0 && !gHeatDirty[old]) {
            gHeatDirty[old] = 1;
            gDirtyCount++;
        }
        if (gHeatTarget >= 0 && !gHeatDirty[gHeatTarget]) {
            gHeatDirty[gHeatTarget] = 1;
            gDirtyCount++;
        }
    }
    gHeatSamples = (gAiLive && g.ai.done) ? g.ai.samples : 0xFFFF;   /* (0xFFFF: draw it when ready) */
    if (force) {
        memset(gHeatDirty, 0, sizeof gHeatDirty);
        gDirtyCount = 0;
        draw_grid_cells(ME);
    }
    draw_think();
}

/* redraw up to n cells whose shade changed */
static void heat_draw(int n)
{
    int i;
    for (i = 0; i < NCELL && gDirtyCount && n; i++)
        if (gHeatDirty[i]) {
            gHeatDirty[i] = 0;
            gDirtyCount--;
            n--;
            draw_cell(ME, i);
        }
}

static void set_think(int on)
{
    gPrefs.think = (uint8_t)on;
    update_menus();
    prefs_save();
    if (on) {
        refresh_heat(1);
    } else {
        gHeatTarget = -1;
        memset(gHeatDirty, 0, sizeof gHeatDirty);
        gDirtyCount = 0;
        draw_grid_cells(ME);
        draw_think();
    }
}

/* ---- a game --------------------------------------------------------------------------------- */

static uint8_t current_rules(void)
{
    return (uint8_t)((gPrefs.salvo ? RULE_SALVO : 0) | (gPrefs.notouch ? RULE_NOTOUCH : 0));
}

static void new_game(uint32_t seed)
{
    g.state = ST_PLACE;
    g.rules = current_rules();
    g.level = gPrefs.level;
    g.rng = seed ? seed : (TICKS * 2654435761UL) ^ 0x6A09E667UL;
    if (!g.rng)
        g.rng = 1;
    fleet_random(g.mine, g.rules, &g.rng);
    fleet_build(&g.fleet[ME], g.mine, g.rules);
    memset(&g.fleet[MAC], 0, sizeof g.fleet[MAC]);
    know_init(&g.know[ME], g.rules);
    know_init(&g.know[MAC], g.rules);
    memset(g.sel, 0, sizeof g.sel);
    g.nsel = 0;
    g.aim = -1;
    g.winner = -1;
    g.myShots = g.macShots = 0;
    g.nFirst = 0;
    dbg.winner = dbg.myShots = dbg.macShots = 0;
    gAiLive = 0;
    gHeatTarget = -1;
    memset(gHeat, 0, sizeof gHeat);
    draw_all();
    say("Place your fleet, Admiral.", 0, 0);
}

static void start_battle(void)
{
    Place mac[NSHIP];
    ai_place_fleet(mac, g.rules, &g.rng, gPrefs.early);
    fleet_build(&g.fleet[MAC], mac, g.rules);
    ai_init(&g.ai, g.level, g.rng ^ 0x5BD1E995UL);
    ai_new_state(&g.ai, &g.know[ME]);
    gAiLive = 1;
    gHeatSamples = 0xFFFF;
    g.state = ST_PLAYER;
    g.aim = 44;
    snd_play(SND_PING);
    draw_all();
    say(g.rules & RULE_SALVO ? "Salvo rules: one shot per ship afloat. Fire away!" : "My fleet is hidden. Fire when ready!", 1, 1);
}

static void set_level(int lvl)
{
    gPrefs.level = (uint8_t)lvl;
    update_menus();
    prefs_save();
    if (g.state == ST_PLAYER || g.state == ST_BUSY) {
        /* the new level takes over at once, from what the Mac knows */
        g.level = (uint8_t)lvl;
        {
            uint32_t s = g.ai.rng;
            ai_init(&g.ai, lvl, s);
        }
        ai_new_state(&g.ai, &g.know[ME]);
        gAiLive = 1;
        gHeatSamples = 0xFFFF;
        refresh_heat(1);
    } else {
        g.level = (uint8_t)lvl;
    }
    draw_think();
}

static void set_rules_option(int which)
{
    if (which == O_SALVO)
        gPrefs.salvo ^= 1;
    else
        gPrefs.notouch ^= 1;
    update_menus();
    prefs_save();
    if (g.state == ST_PLACE || g.state == ST_OVER) {
        new_game(0);
    } else {
        say("New rules from the next game on.", 0, 0);
    }
}

static void game_over(int winner)
{
    g.winner = (short)winner;
    g.state = ST_OVER;
    gAiLive = 0;
    gHeatTarget = -1;
    stats_record(winner == ME, g.myShots, g.macShots, g.level, g.firstShots, g.nFirst);
    dbg.winner = (uint32_t)winner + 1;
    draw_all();
    snd_play(winner == ME ? SND_WIN : SND_LOSE);
    if (winner == ME)
        say("You sank my whole fleet! Well played, Admiral.", 2, 1);
    else
        say("Victory is mine! Better luck next time.", 3, 1);
}

/* resolve shots by `shooter` (ME fires at the Mac's grid, and the other way round) */
static void resolve(int shooter, const uint8_t *cells, int n)
{
    int target = shooter == ME ? MAC : ME, i, hits = 0, sinks = 0, lastSunk = -1;
    for (i = 0; i < n; i++) {
        int sunk = -1, res = fire(&g.fleet[target], &g.know[target], cells[i], &sunk);
        if (res == SHOT_BAD)
            continue;
        if (shooter == ME) {
            g.myShots++;
            if (g.nFirst < 25)
                g.firstShots[g.nFirst++] = cells[i];
        } else {
            g.macShots++;
        }
        dbg.myShots = (uint32_t)g.myShots;
        dbg.macShots = (uint32_t)g.macShots;
        fx_shot(target, cells[i], res);
        if (res != SHOT_MISS)
            hits++;
        if (res == SHOT_SUNK) {
            char msg[80];
            sinks++;
            lastSunk = sunk;
            fx_sink(target, sunk);
            msg[0] = 0;
            if (shooter == ME) {
                str_cat(msg, "You sank my ");
                str_cat(msg, ship_name(sunk));
                str_cat(msg, "!");
                say(msg, 2, 1);
            } else {
                str_cat(msg, "Down goes your ");
                str_cat(msg, ship_name(sunk));
                str_cat(msg, "!");
                say(msg, 3, 1);
            }
            pump(20);
        }
        draw_bar();
        if (!g.fleet[target].afloat)
            return;
    }
    if (sinks)
        return;
    (void)lastSunk;
    if (n > 1) {
        char msg[48];
        if (shooter == ME) {
            strcpy(msg, hits ? (hits == 1 ? "One hit. Ouch." : "Several hits! Ouch!") : "All misses. Ha!");
            say(msg, hits ? 2 : 3, 0);
        } else {
            strcpy(msg, hits ? (hits == 1 ? "One hit for me!" : "Several hits for me!") : "All misses. Hmm.");
            say(msg, hits ? 3 : 0, 0);
        }
    } else if (shooter == ME) {
        say(hits ? PICK(kMyHit) : PICK(kMyMiss), hits ? 2 : 3, 0);
    } else {
        say(hits ? PICK(kMacHit) : PICK(kMacMiss), hits ? 3 : 0, hits);
    }
}

static void mac_turn(void)
{
    uint8_t cells[5];
    int n = (g.rules & RULE_SALVO) ? g.fleet[MAC].afloat : 1, i;
    unsigned long t0 = TICKS, dt;
    draw_titles_now();
    draw_bar();
    /* finish the analysis (most of it was done while I was thinking); on a slow machine stop
       after about 0.75 s once there are a few dozen fleets: the choice hardly changes */
    while (!ai_work(&g.ai, &g.know[ME], 4)) {
        SystemTask();
        if (TICKS - t0 > 45 && g.ai.good >= 24) {
            g.ai.done = 1;
            break;
        }
    }
    n = ai_choose(&g.ai, &g.know[ME], cells, n);
    dt = TICKS - t0;
    dbg.aiTicksLast = dt;
    if (dt > dbg.aiTicksMax)
        dbg.aiTicksMax = dt;
    dbg.aiTicksSum += dt;
    dbg.aiMoves++;
    dbg.aiSamples = g.ai.samples;
    dbg.aiExact = g.ai.exact;
    gAiLive = 0;
    gHeatTarget = -1;
    /* show where it aims */
    for (i = 0; i < n; i++)
        draw_reticle(ME, cells[i], 1);
    pump((short)(dt < 25 ? 25 - dt : 4));
    for (i = 0; i < n; i++)
        draw_cell(ME, cells[i]);
    resolve(MAC, cells, n);
    ai_new_state(&g.ai, &g.know[ME]);
    gAiLive = 1;
    gHeatSamples = 0xFFFF;              /* (the new analysis is drawn once it is ready) */
    if (gPrefs.think) {
        memset(gHeat, 0, sizeof gHeat);
        refresh_heat(1);
    }
}

static void player_fire(int cell)
{
    uint8_t cells[5];
    int n = 0, i;
    if (g.state != ST_PLAYER || cell < 0 || g.know[MAC].cell[cell] != K_UNKNOWN) {
        if (cell >= 0 && g.state == ST_PLAYER)
            SysBeep(2);
        return;
    }
    if (g.aim != cell) {
        int old = g.aim;
        g.aim = (short)cell;
        if (old >= 0)
            draw_cell(MAC, old);
    }
    if (g.rules & RULE_SALVO) {
        g.sel[cell] ^= 1;
        g.nsel = (short)(g.nsel + (g.sel[cell] ? 1 : -1));
        draw_cell(MAC, cell);
        snd_play(SND_CLICK);
        if (g.nsel < g.fleet[ME].afloat) {
            draw_bar();
            return;
        }
        for (i = 0; i < NCELL; i++)
            if (g.sel[i]) {
                cells[n++] = (uint8_t)i;
                g.sel[i] = 0;
            }
        g.nsel = 0;
    } else {
        cells[n++] = (uint8_t)cell;
    }
    g.state = ST_BUSY;
    dbg.state = (uint32_t)g.state + 1;
    draw_titles_now();
    draw_bar();
    for (i = 0; i < n; i++)
        draw_cell(MAC, cells[i]);
    resolve(ME, cells, n);
    if (!g.fleet[MAC].afloat) {
        game_over(ME);
        return;
    }
    pump(15);
    mac_turn();
    if (!g.fleet[ME].afloat) {
        game_over(MAC);
        return;
    }
    g.state = ST_PLAYER;
    dbg.state = (uint32_t)g.state + 1;
    FlushEvents(mDownMask | mUpMask | keyDownMask | autoKeyMask, 0);   /* clicks made while busy */
    draw_titles_now();
    draw_bar();
    draw_cell(MAC, g.aim);
}

/* ---- placing my fleet ----------------------------------------------------------------------- */

static int try_place(int s, Place p)
{
    Place t[NSHIP];
    memcpy(t, g.mine, sizeof t);
    t[s] = p;
    if (!fleet_valid(t, g.rules))
        return 0;
    memcpy(g.mine, t, sizeof t);
    fleet_build(&g.fleet[ME], g.mine, g.rules);
    return 1;
}

static void redraw_my_grid(void)
{
    draw_grid_cells(ME);
}

static void rotate_ship(int s, int pivotCell)
{
    Place p = g.mine[s];
    int len = kShipLen[s], k;
    /* turn about the clicked cell; if it doesn't fit there, slide along the new direction */
    int off = p.dir ? kRow[pivotCell] - p.r : kCol[pivotCell] - p.c;
    p.dir ^= 1;
    if (p.dir) {
        p.r = (uint8_t)(kRow[pivotCell] - off < 0 ? 0 : kRow[pivotCell] - off);
        p.c = (uint8_t)kCol[pivotCell];
    } else {
        p.c = (uint8_t)(kCol[pivotCell] - off < 0 ? 0 : kCol[pivotCell] - off);
        p.r = (uint8_t)kRow[pivotCell];
    }
    for (k = 0; k < 10; k++) {
        Place q = p;
        int d = (k & 1) ? -(k + 1) / 2 : k / 2;
        if (q.dir) {
            int r = p.r + d;
            if (r < 0 || r + len > 10)
                continue;
            q.r = (uint8_t)r;
        } else {
            int c = p.c + d;
            if (c < 0 || c + len > 10)
                continue;
            q.c = (uint8_t)c;
        }
        if (try_place(s, q)) {
            snd_play(SND_CLICK);
            redraw_my_grid();
            return;
        }
    }
    SysBeep(2);
}

static void drag_ship(int s, Point start)
{
    Place p0 = g.mine[s], cur = p0;
    int len = kShipLen[s], cell = cell_at(ME, start), dr = kRow[cell] - p0.r, dc = kCol[cell] - p0.c, moved = 0;
    Rect r;
    place_rect(ME, &cur, len, &r);
    draw_drag(&r);
    while (StillDown()) {
        Point p;
        int c, row, col;
        Place q = cur;
        GetMouse(&p);
        c = (short)((p.h - gGridR[ME].left - 1 + CS * 20) / CS) - 20;
        row = (short)((p.v - gGridR[ME].top - 1 + CS * 20) / CS) - 20;
        col = c - dc;
        row = row - dr;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        if (cur.dir) {
            if (row > 10 - len) row = 10 - len;
            if (col > 9) col = 9;
        } else {
            if (col > 10 - len) col = 10 - len;
            if (row > 9) row = 9;
        }
        q.r = (uint8_t)row;
        q.c = (uint8_t)col;
        if (q.r != cur.r || q.c != cur.c) {
            draw_drag(&r);
            cur = q;
            place_rect(ME, &cur, len, &r);
            draw_drag(&r);
            moved = 1;
        }
    }
    draw_drag(&r);
    if (!moved || (cur.r == p0.r && cur.c == p0.c)) {
        if (!moved)
            rotate_ship(s, cell);
        return;
    }
    if (try_place(s, cur)) {
        snd_play(SND_CLICK);
        redraw_my_grid();
    } else {
        SysBeep(2);
    }
}

static void randomize_mine(void)
{
    fleet_random(g.mine, g.rules, &g.rng);
    fleet_build(&g.fleet[ME], g.mine, g.rules);
    redraw_my_grid();
}

/* ---- windows: About, Statistics ------------------------------------------------------------- */

static void modal_text(const char *const *lines, short n, short width)
{
    WindowPtr w;
    Rect r, b = qd.screenBits.bounds;
    EventRecord ev;
    short i, h = (short)(24 + n * 12);
    SetRect(&r, (short)((b.left + b.right - width) / 2), 50, (short)((b.left + b.right + width) / 2), (short)(50 + h));
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    SetPort(w);
    for (i = 0; i < n; i++) {
        char *s = (char *)lines[i];
        short bold = s[0] == '*';
        if (bold)
            s++;
        TextFont(bold ? 0 : 3);
        TextSize(bold ? 0 : 9);
        MoveTo(14, (short)(18 + i * 12));
        DrawText(s, 0, (short)strlen(s));
    }
    for (;;) {
        SystemTask();
        if (GetNextEvent(mDownMask | keyDownMask, &ev))
            break;
    }
    DisposeWindow(w);
    SetPort(gWin);
}

static void about(void)
{
    static const char *const lines[] = {
        "*Broadside",
        "Sink the Mac's fleet before it sinks yours.",
        "",
        "Easy hunts at random. Normal counts where each",
        "ship could be. Hard works out the exact chance that",
        "each square holds a ship, over every fleet that fits",
        "what it has seen: it counts them all when there are",
        "few enough and samples them when there are billions.",
        "Options > Show the Mac's Thinking shades your grid.",
        "",
        "Carrier 5, Dreadnought 4, Cruiser 3, Submarine 3,",
        "Destroyer 2. R turns a ship, Tab scatters them,",
        "Return starts. W A S D or arrows aim, Space fires.",
        "T: thinking, M: sound, Esc: quit.",
        "",
        "Original game, art and sounds. Click to continue.",
    };
    modal_text(lines, (short)(sizeof lines / sizeof lines[0]), 300);
}

static void num_cat(char *d, unsigned long n)
{
    char t[12];
    short i = 0, j = (short)strlen(d);
    do {
        t[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i)
        d[j++] = t[--i];
    d[j] = 0;
}

static void dec1_cat(char *d, unsigned long num, unsigned long den)
{
    unsigned long q = den ? (num * 10 + den / 2) / den : 0;
    num_cat(d, q / 10);
    strcat(d, ".");
    num_cat(d, q % 10);
}

static void statistics(void)
{
    static char buf[3][4][64];
    const char *lines[20];
    static const char *const names[3] = {"Easy", "Normal", "Hard"};
    short n = 0, l;
    lines[n++] = "*Statistics";
    for (l = 0; l < 3; l++) {
        char *a = buf[l][0], *b = buf[l][1], *c = buf[l][2], *d = buf[l][3];
        lines[n++] = "";
        strcpy(a, "*");
        strcat(a, names[l]);
        strcpy(b, "Played ");
        num_cat(b, gPrefs.st[l].games);
        strcat(b, ", won ");
        num_cat(b, gPrefs.st[l].wins);
        strcat(b, ", lost ");
        num_cat(b, gPrefs.st[l].losses);
        if (gPrefs.st[l].games) {
            strcat(b, " (");
            num_cat(b, (gPrefs.st[l].wins * 100UL + gPrefs.st[l].games / 2) / gPrefs.st[l].games);
            strcat(b, "% won)");
        }
        strcpy(c, "Your shots to win: ");
        if (gPrefs.st[l].wins) {
            dec1_cat(c, gPrefs.st[l].myShotsWon, gPrefs.st[l].wins);
            strcat(c, " on average, best ");
            num_cat(c, gPrefs.st[l].best);
        } else {
            strcat(c, "no wins yet");
        }
        strcpy(d, "The Mac's shots to win: ");
        if (gPrefs.st[l].losses)
            dec1_cat(d, gPrefs.st[l].macShotsWon, gPrefs.st[l].losses);
        else
            strcat(d, "no wins yet");
        lines[n++] = a;
        lines[n++] = b;
        lines[n++] = c;
        lines[n++] = d;
    }
    lines[n++] = "";
    lines[n++] = "Click to continue.";
    modal_text(lines, n, 300);
}

/* ---- menus and keys ------------------------------------------------------------------------- */

static void do_menu(long mr)
{
    short menu = (short)(mr >> 16), item = (short)mr;
    switch (menu) {
    case M_APPLE:
        if (item == 1) {
            about();
        } else {
            Str255 name;
            GrafPtr old;
            GetPort(&old);
            GetMenuItemText(mApple, item, name);
            OpenDeskAcc(name);
            SetPort(old);
        }
        break;
    case M_FILE:
        if (item == F_NEW)
            new_game(0);
        else if (item == F_STATS)
            statistics();
        else if (item == F_QUIT)
            quit_app();
        break;
    case M_OPTS:
        if (item >= O_EASY && item <= O_HARD)
            set_level(item - O_EASY);
        else if (item == O_SALVO || item == O_NOTOUCH)
            set_rules_option(item);
        else if (item == O_THINK)
            set_think(!gPrefs.think);
        else if (item == O_SOUND)
            toggle_sound();
        else if (item == O_SPEECH) {
            gPrefs.speech ^= 1;
            update_menus();
            prefs_save();
        }
        break;
    }
    HiliteMenu(0);
}

static void move_aim(int dr, int dc)
{
    int old = g.aim, r, c;
    if (old < 0)
        old = 44;
    r = kRow[old] + dr;
    c = kCol[old] + dc;
    if (r < 0 || r > 9 || c < 0 || c > 9)
        return;
    g.aim = (short)(r * 10 + c);
    draw_cell(MAC, old);
    draw_cell(MAC, g.aim);
}

static void do_key(const EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    if (is_esc(ev)) {
        quit_app();
        return;
    }
    if (ch >= 'A' && ch <= 'Z')
        ch = (char)(ch + 32);
    if (ch == 'm') {
        toggle_sound();
        return;
    }
    if (ch == 't') {
        set_think(!gPrefs.think);
        return;
    }
    if (g.state == ST_PLACE) {
        if (ch == 'r') {
            Point p;
            int c;
            GetMouse(&p);
            c = cell_at(ME, p);
            if (c >= 0 && g.fleet[ME].owner[c])
                rotate_ship(g.fleet[ME].owner[c] - 1, c);
        } else if (ch == '\t') {
            randomize_mine();
        } else if (ch == '\r' || ch == 3) {
            start_battle();
        }
    } else if (g.state == ST_PLAYER) {
        if (ch == 'w' || ch == 0x1E)
            move_aim(-1, 0);
        else if (ch == 's' || ch == 0x1F)
            move_aim(1, 0);
        else if (ch == 'a' || ch == 0x1C)
            move_aim(0, -1);
        else if (ch == 'd' || ch == 0x1D)
            move_aim(0, 1);
        else if (ch == ' ' || ch == '\r' || ch == 3)
            player_fire(g.aim);
    } else if (g.state == ST_OVER) {
        if (ch == '\r' || ch == 3 || ch == ' ')
            new_game(0);
    }
}

static void do_click(Point p)
{
    int b = button_at(p), c;
    if (b) {
        track_button(b);
        {
            Point q;
            GetMouse(&q);
            if (!PtInRect(q, &gBtnR[b]))
                return;
        }
        if (b == BTN_RANDOM)
            randomize_mine();
        else if (b == BTN_START)
            start_battle();
        else if (b == BTN_NEW)
            new_game(0);
        return;
    }
    if (g.state == ST_PLACE) {
        c = cell_at(ME, p);
        if (c >= 0 && g.fleet[ME].owner[c])
            drag_ship(g.fleet[ME].owner[c] - 1, p);
    } else if (g.state == ST_PLAYER) {
        c = cell_at(MAC, p);
        if (c >= 0)
            player_fire(c);
    }
}

/* ---- the test mailbox and the AI benchmark -------------------------------------------------- */

/* Hard (or arg level) against random fleets, exactly as host/sim.c plays them (same seeds), so
   the shots per game must match the host's. Results in dbg.bench. */
static void bench(int games, int level, int samples)
{
    static AI a;
    int gi;
    unsigned long moves = 0, ticks = 0, maxT = 0, sampSum = 0;
    for (gi = 0; gi < games && gi < 24; gi++) {
        Fleet f;
        Know k;
        Place ships[NSHIP];
        uint32_t rng = 0x9E3779B9UL ^ ((uint32_t)gi * 2654435761UL) ^ 12345UL;
        if (!rng)
            rng = 1;
        fleet_random(ships, 0, &rng);
        fleet_build(&f, ships, 0);
        know_init(&k, 0);
        ai_init(&a, level, rng ^ 0xA5A5A5A5UL);
        if (samples)
            a.minSamples = (uint16_t)samples;
        while (f.afloat) {
            uint8_t cell;
            int sunk;
            unsigned long t0 = TICKS, dt;
            ai_new_state(&a, &k);
            while (!ai_work(&a, &k, 50))
                ;
            ai_choose(&a, &k, &cell, 1);
            dt = TICKS - t0;
            ticks += dt;
            if (dt > maxT)
                maxT = dt;
            moves++;
            sampSum += a.samples;
            fire(&f, &k, cell, &sunk);
            SystemTask();
        }
        dbg.bench[gi] = k.shots;
        dbg.bench[24] = moves;
        dbg.bench[25] = ticks;
        dbg.bench[26] = maxT;
        dbg.bench[27] = sampSum;
        dbg.bench[28] = (uint32_t)gi + 1;
    }
}

static void mailbox(void)
{
    uint32_t c = dbg.cmd;
    if (!c)
        return;
    dbg.cmdResult = 0;
    switch (c) {
    case 1:     /* new game: seed, level, rules; random fleet; start the battle */
        gPrefs.level = (uint8_t)dbg.arg[1];
        gPrefs.salvo = (uint8_t)((dbg.arg[2] & RULE_SALVO) ? 1 : 0);
        gPrefs.notouch = (uint8_t)((dbg.arg[2] & RULE_NOTOUCH) ? 1 : 0);
        update_menus();
        new_game(dbg.arg[0]);
        start_battle();
        break;
    case 2:     /* I fire at arg0 */
        player_fire((int)dbg.arg[0]);
        break;
    case 3:     /* thinking on/off */
        set_think(dbg.arg[0] != 0);
        break;
    case 4:     /* benchmark: games, level, samples */
        bench((int)dbg.arg[0], (int)dbg.arg[1], (int)dbg.arg[2]);
        break;
    case 6:     /* autopilot: my next shot (or whole salvo) chosen by a Normal AI */
        if (g.state == ST_PLAYER) {
            static AI pilot;
            uint8_t cells[5];
            int n = (g.rules & RULE_SALVO) ? g.fleet[ME].afloat - g.nsel : 1, i;
            ai_init(&pilot, AI_NORMAL, g.rng ^ 0x1234567UL);
            ai_new_state(&pilot, &g.know[MAC]);
            ai_work(&pilot, &g.know[MAC], 1);
            n = ai_choose(&pilot, &g.know[MAC], cells, n);
            for (i = 0; i < n; i++)
                player_fire(cells[i]);
        }
        break;
    case 9:     /* quit (the boot floppies relaunch the app: it is the shell) */
        dbg.cmd = 0;
        quit_app();
        break;
    case 7:     /* About box (waits for a click) */
        dbg.cmdResult = 7;
        about();
        break;
    case 8:     /* Statistics (waits for a click) */
        dbg.cmdResult = 8;
        statistics();
        break;
    case 5:     /* sound on/off */
        gPrefs.sound = (uint8_t)(dbg.arg[0] != 0);
        update_menus();
        break;
    }
    dbg.cmdDone++;
    dbg.cmd = 0;                       /* (cleared when done: the tests wait for it) */
}

/* ---- main ----------------------------------------------------------------------------------- */

static void idle(void)
{
    Point p;
    SetPort(gWin);
    if (g.state == ST_PLAYER) {
        if (gAiLive && g.ai.samples < 4 * g.ai.minSamples)
            ai_work(&g.ai, &g.know[ME], 3);
        if (gPrefs.think && TICKS >= gHeatNext && g.ai.done && g.ai.samples != gHeatSamples && !gDirtyCount) {
            refresh_heat(0);
            gHeatNext = TICKS + 30;
        }
        if (gDirtyCount)
            heat_draw(8);
        fx_idle();
    } else if (g.state == ST_OVER) {
        fx_idle();
    }
    GetMouse(&p);
    (void)p;
}

int main(void)
{
    EventRecord ev;
    WindowPtr w;
    Rect r;
    short part;

    dbg.magic = DBG_MAGIC;
    dbg.version = 1;
    dbg.notMagic = ~DBG_MAGIC;
    dbg.anchor = (uint32_t)&ai_work;

    MaxApplZone();
    MoreMasters();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    prefs_load();
    dbg.speech = (uint32_t)speech_init();
    snd_init();
    layout();
    setup_menus();
    r = qd.screenBits.bounds;
    gWin = NewWindow(NULL, &r, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    SetPort(gWin);
    new_game(0);
    dbg.freeMem = (uint32_t)FreeMem();

    for (;;) {
        dbg.loops++;
        dbg.state = (uint32_t)g.state + 1;
        dbg.bench[30] = gPrefs.think;
        SystemTask();
        mailbox();
        if (!GetNextEvent(everyEvent, &ev)) {
            idle();
            continue;
        }
        switch (ev.what) {
        case mouseDown:
            part = FindWindow(ev.where, &w);
            if (part == inMenuBar) {
                do_menu(MenuSelect(ev.where));
            } else if (part == inSysWindow) {
                SystemClick(&ev, w);
            } else if (w == gWin) {
                SetPort(gWin);
                do_click(ev.where);
            }
            break;
        case keyDown:
        case autoKey:
            if (ev.modifiers & cmdKey) {
                if (ev.what == keyDown)
                    do_menu(MenuKey((char)(ev.message & charCodeMask)));
            } else if (ev.what == keyDown || g.state == ST_PLAYER) {
                SetPort(gWin);
                do_key(&ev);
            }
            break;
        case updateEvt:
            w = (WindowPtr)ev.message;
            BeginUpdate(w);
            if (w == gWin)
                draw_all();
            EndUpdate(w);
            break;
        default:
            break;
        }
    }
    return 0;
}
