/*
 * Neural Pinball rules. The table is about building an AI:
 *
 *   T-R-A-I-N lanes   roll through all five letters to finish an EPOCH: the bonus multiplier
 *                     goes up (2x .. 5x) and the kickback is relit. Flippers move the lit
 *                     letters (lane change). The lane blinking at launch is the SKILL SHOT.
 *   Neurons           the three pop bumpers.
 *   DROPOUT           knock down all three drop targets to light LOCK at the bowl.
 *   GRADIENT DESCENT  the bowl: slow balls spiral into its minimum ("CONVERGED"). With LOCK lit
 *                     the ball is locked (virtual lock); the second lock starts
 *   OVERFIT MULTIBALL three balls; the BACKPROP ramp scores the JACKPOT, after which the bowl
 *                     holds the SINGULARITY (super jackpot) for 15 s.
 *   ATTENTION         the spinner in the left orbit.
 *   TURING TEST       make the ramp, the orbit and the drop bank once each to light it at the
 *                     bowl; then convince the robot judge with 3 lit shots in 30 s.
 *   Ball save, kickback, tilt (three warnings), ball search, end-of-ball bonus x epoch.
 *
 * All integer, deterministic: the same buttons give the same game on the host and the Mac.
 */
#include "game.h"

#define TICKS_PER_S 60
#define FX(px) ((fx)((px) * 65536L))

static uint32_t rnd(Game *g)
{
    g->rng = g->rng * 1103515245UL + 12345UL;
    return (g->rng >> 8) & 0xFFFFFF;
}

static void copy_str(char *d, const char *s)
{
    int16_t i = 0;
    while (s && s[i] && i < MSG_LEN - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

void fmt_score(int32_t v, char *out)
{
    char tmp[16];
    int16_t n = 0, i, k = 0;
    if (v < 0)
        v = 0;
    do {
        if (n && n % 3 == 0)
            tmp[k++] = ',';
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
        n++;
    } while (v);
    for (i = 0; i < k; i++)
        out[i] = tmp[k - 1 - i];
    out[k] = 0;
}

void game_message(Game *g, const char *top, const char *bottom, int16_t ticks, int16_t big)
{
    copy_str(g->msg[0], top);
    copy_str(g->msg[1], bottom);
    g->msg_t = ticks;
    g->msg_big = big;
}

static void add_score(Game *g, int32_t pts)
{
    if (g->state != GS_PLAY || g->tilted)
        return;
    g->score += pts;
}

static void say(Game *g, int16_t s)
{
    if (!g->say)
        g->say = s;
}

/* ---------------------------------------------------------------------------- setup */

void game_init(Game *g, uint32_t seed)
{
    char *p = (char *)g;
    uint32_t i;
    for (i = 0; i < sizeof(Game); i++)
        p[i] = 0;
    g->rng = seed ? seed : 12345;
    phys_init(&g->w, table_shapes, NSHAPES, table_mats, TABLE_W * 64, TABLE_H * 64);
    phys_add_flipper(&g->w, &table_flippers[0]);
    phys_add_flipper(&g->w, &table_flippers[1]);
    phys_set_plunger(&g->w, PLUNGER_X0, PLUNGER_X1, PLUNGER_Y, PLUNGER_PULL, PLUNGER_RATE, PLUNGER_SPEED);
    phys_set_ramp_box(&g->w, RAMP_BOX);
    ai_init(&g->ai, seed ^ 0x5A5A5A5AUL);
    g->sh_kickback = phys_find(&g->w, S_LINE, ID_KICKBACK);
    g->sh_drop0 = phys_find(&g->w, S_SEG, ID_DROP0);
    g->state = GS_OVER;
    g->balls_per_game = 3;
    g->skill_lane = -1;
}

static void drops_reset(Game *g)
{
    int16_t k;
    for (k = 0; k < 3; k++)
        phys_shape_on(&g->w, (int16_t)(g->sh_drop0 + k), 1);
    g->drops_down = 0;
}

static void serve_ball(Game *g)
{
    phys_add_ball(&g->w, BALL_START_X, BALL_START_Y, 0, 0);
    g->launched = 0;
    g->save_armed = 1;
    g->save_t = 0;
    g->skill_lane = (int16_t)(rnd(g) % 5);
}

static void reset_ball_state(Game *g)
{
    g->epoch = 1;
    g->ramps = g->orbits = g->spins = g->bumps = 0;
    g->bonus = 0;
    g->tilt_meter = 0;
    g->tilt_warn = 0;
    g->tilted = 0;
    g->w.tilted = 0;
    g->end_t = 0;
    g->search_t = 0;
}

void game_start(Game *g, int16_t ai)
{
    int16_t i;
    for (i = 0; i < MAX_BALLS; i++)
        g->w.ball[i].state = BALL_OFF;
    g->state = GS_PLAY;
    g->ai_playing = ai;
    g->score = 0;
    g->ball = 1;
    g->lanes = 0;
    g->kickback = 1;
    g->lock_lit = 0;
    g->locks = 0;
    g->multiball = 0;
    g->jackpot_lit = 0;
    g->super_t = 0;
    g->jackpots = 0;
    g->turing_lit = g->turing_t = g->turing_hits = g->turing_targets = 0;
    g->face = FACE_OFF;
    g->new_hiscore = 0;
    drops_reset(g);
    reset_ball_state(g);
    for (i = 0; i < MAX_BALLS; i++)
        g->held_t[i] = 0;
    serve_ball(g);
    if (ai) {
        ai_init(&g->ai, rnd(g) + 1);
        game_message(g, "AI PLAYING", "PRESS RETURN", 180, 0);
    } else
        game_message(g, "BALL 1", "PULL THE PLUNGER", 150, 0);
}

void game_takeover(Game *g)
{
    if (g->state != GS_PLAY || !g->ai_playing)
        return;
    g->ai_playing = 0;
    g->score = 0;
    g->ball = 1;
    g->epoch = 1;
    g->lanes = 0;
    g->locks = 0;
    g->ramps = g->orbits = g->spins = g->bumps = 0;
    game_message(g, "YOU HAVE CONTROL", "", 150, 1);
}

/* ---------------------------------------------------------------------------- scoring */

static void lane_lit(Game *g, int16_t lane)
{
    if (g->lanes & (1 << lane)) {
        add_score(g, 500);
        return;
    }
    g->lanes |= (int16_t)(1 << lane);
    add_score(g, 5000);
    g->sounds |= SND_LANE;
    if (g->lanes == 0x1F) {
        g->lanes = 0;
        if (g->epoch < 5)
            g->epoch++;
        g->kickback = 1;
        phys_arm(&g->w, g->sh_kickback, 1);
        add_score(g, 25000);
        {
            char t[MSG_LEN] = "EPOCH COMPLETE";
            char b[MSG_LEN] = "BONUS x1";
            b[7] = (char)('0' + g->epoch);
            game_message(g, t, b, 120, 1);
        }
        g->sounds |= SND_FANFARE;
        say(g, SAY_TRAINED);
    }
}

static void turing_shot(Game *g, int16_t which)
{
    /* which: 1 ramp, 2 orbit, 4 drops */
    if (g->turing_t > 0) {
        if (g->turing_targets == which)
            return;                         /* never the same shot twice in a row */
        g->turing_targets = which;          /* (during the test: the shot just made, unlit) */
        g->turing_hits++;
        add_score(g, 50000L * g->turing_hits);
        g->face = g->turing_hits >= 2 ? FACE_HAPPY : FACE_ON;
        g->face_t = 0;
        if (g->turing_hits >= g->turing_need) {
            g->turing_t = 0;
            g->turing_targets = 0;
            add_score(g, 500000L);
            g->face = FACE_HAPPY;
            g->face_t = 240;
            game_message(g, "TURING TEST", "PASSED! 500,000", 180, 1);
            g->sounds |= SND_FANFARE;
            say(g, SAY_PASSED);
        } else
            game_message(g, "JUDGE CONVINCED", g->turing_hits == 1 ? "1 OF 3" : "2 OF 3", 90, 0);
        return;
    }
    if (g->turing_lit)
        return;
    g->turing_targets |= which;
    if (g->turing_targets == 7) {
        g->turing_lit = 1;
        g->turing_targets = 0;
        g->face = FACE_ON;
        game_message(g, "TURING TEST", "IS LIT AT THE BOWL", 120, 0);
    }
}

static void start_multiball(Game *g)
{
    int16_t k;
    g->multiball = 1;
    g->jackpot_lit = 1;
    g->locks = 0;
    for (k = 0; k < 2; k++) {
        int16_t b = phys_add_ball(&g->w, BALL_START_X, FX(200 + k * 40), 0, -FX(12) - 120000L);
        (void)b;
    }
    game_message(g, "OVERFIT", "MULTIBALL!", 180, 1);
    g->sounds |= SND_MULTI;
    say(g, SAY_MULTIBALL);
    g->save_t = 15 * TICKS_PER_S;
}

static void sink_ball(Game *g, int16_t bi)
{
    add_score(g, 15000);
    g->sounds |= SND_SINK;
    g->held_t[bi] = 70;
    if (g->super_t > 0) {
        g->super_t = 0;
        add_score(g, 1000000L * g->epoch);
        game_message(g, "SINGULARITY", "1,000,000 x EPOCH", 200, 1);
        g->face = FACE_SURPRISED;
        g->face_t = 200;
        g->sounds |= SND_JACKPOT;
        say(g, SAY_SINGULARITY);
        g->jackpot_lit = g->multiball != 0;
        return;
    }
    if (g->turing_lit && !g->turing_t && !g->multiball) {
        g->turing_lit = 0;
        g->turing_t = 40 * TICKS_PER_S;
        g->turing_hits = 0;
        g->turing_need = 3;
        g->turing_targets = 0;
        g->face = FACE_SUSPICIOUS;
        g->held_t[bi] = 120;
        game_message(g, "TURING TEST", "CONVINCE THE JUDGE", 150, 1);
        g->sounds |= SND_FANFARE;
        say(g, SAY_TURING);
        return;
    }
    if (g->lock_lit && !g->multiball) {
        g->lock_lit = 0;
        g->locks++;
        if (g->locks >= 2) {
            g->held_t[bi] = 90;
            start_multiball(g);
        } else
            game_message(g, "BALL LOCKED", "DROPOUT FOR LOCK 2", 120, 0);
        return;
    }
    game_message(g, "CONVERGED", "15,000", 70, 0);
    say(g, SAY_CONVERGED);
}

static void end_of_ball(Game *g);

static void handle_event(Game *g, const Event *e)
{
    int16_t id = e->id;
    switch (e->type) {
    case EV_BUMPER:
        g->skill_t = 0;                         /* the skill shot is only for the plunge */
        add_score(g, g->multiball ? 2000 : 1000);
        g->bumps++;
        g->sounds |= SND_BUMPER;
        if (id >= ID_BUMPER0 && id <= ID_BUMPER2)
            g->bump_flash |= (uint8_t)(1 << (id - ID_BUMPER0));
        break;
    case EV_SLING:
        g->skill_t = 0;
        add_score(g, 100);
        g->sounds |= SND_SLING;
        break;
    case EV_DROP:
        if (id >= ID_DROP0 && id <= ID_DROP2) {
            g->drops_down |= (int16_t)(1 << (id - ID_DROP0));
            add_score(g, 3000);
            g->sounds |= SND_DROP;
            if (g->drops_down == 7) {
                add_score(g, 20000);
                g->drop_reset_t = 90;
                if (!g->multiball && !g->lock_lit) {
                    g->lock_lit = 1;
                    game_message(g, "DROPOUT!", "LOCK IS LIT", 120, 0);
                } else
                    game_message(g, "DROPOUT!", "20,000", 90, 0);
                say(g, SAY_DROPOUT);
                turing_shot(g, 4);
            }
        }
        break;
    case EV_SENSOR:
        if (id >= ID_LANE0 && id <= ID_LANE4 && e->val > 0) {
            int16_t lane = (int16_t)(id - ID_LANE0);
            if (g->skill_lane == lane && g->skill_t > 0) {
                add_score(g, 50000);
                game_message(g, "SKILL SHOT", "50,000", 120, 1);
                g->sounds |= SND_FANFARE;
                say(g, SAY_SKILL);
            }
            g->skill_lane = -1;
            g->skill_t = 0;
            lane_lit(g, lane);
        } else if (id == ID_RAMP_MADE && e->val > 0) {
            g->ramps++;
            g->sounds |= SND_RAMP;
            if (g->jackpot_lit && g->multiball) {
                int32_t jp = 250000L * g->epoch;
                char s[MSG_LEN];
                g->jackpots++;
                add_score(g, jp);
                fmt_score(jp, s);
                game_message(g, "JACKPOT", s, 150, 1);
                g->jackpot_lit = 0;
                g->super_t = 15 * TICKS_PER_S;
                g->face = FACE_SURPRISED;
                g->face_t = 120;
                g->sounds |= SND_JACKPOT;
                say(g, SAY_JACKPOT);
            } else {
                add_score(g, 10000L + 2500L * (g->ramps > 8 ? 8 : g->ramps));
                game_message(g, "BACKPROP", "WEIGHTS UPDATED", 70, 0);
                say(g, SAY_BACKPROP);
            }
            turing_shot(g, 1);
        } else if (id == ID_ORBIT && e->val > 0) {
            g->orbits++;
            add_score(g, 7500);
            g->sounds |= SND_LANE;
            game_message(g, "ATTENTION", "IS ALL YOU NEED", 70, 0);
            turing_shot(g, 2);
        } else if ((id == ID_INLANE_L || id == ID_INLANE_R) && e->val > 0) {
            add_score(g, 1000);
        } else if ((id == ID_OUTLANE_L || id == ID_OUTLANE_R) && e->val > 0) {
            add_score(g, 5000);
        } else if (id == ID_SHOOTER_EXIT && e->val < 0) {
            if (!g->launched) {
                g->launched = 1;
                g->skill_t = 4 * TICKS_PER_S;          /* the skill shot: into the blinking lane */
                g->sounds |= SND_LAUNCH;
                if (g->save_armed) {
                    g->save_t = 12 * TICKS_PER_S;
                    g->save_armed = 0;
                }
            }
        }
        break;
    case EV_SPIN: {
        int16_t n = (int16_t)(1 + e->val / 900);       /* val: q10 px/step */
        g->spins += n;
        add_score(g, 250L * n);
        g->spinner_w = (int16_t)(e->val / 8);
        g->sounds |= SND_SPIN;
        break;
    }
    case EV_SINK:
        sink_ball(g, e->ball);
        break;
    case EV_KICKBACK:
        g->kickback = 0;
        g->sounds |= SND_KICK;
        game_message(g, "KICKBACK", "", 60, 0);
        break;
    case EV_FLIPHIT:
    case EV_WALL:
        g->sounds |= SND_WALL;
        break;
    case EV_DRAIN:
    case EV_ESCAPE:
        g->sounds |= SND_DRAIN;
        if (g->state == GS_PLAY && g->save_t > 0 && !g->tilted) {
            if (g->multiball) {
                phys_add_ball(&g->w, BALL_START_X, FX(250), 0, -FX(12) - 80000L);
            } else {
                serve_ball(g);
                g->save_armed = 0;
                game_message(g, "BALL SAVED", "", 90, 1);
                say(g, SAY_SAVED);
            }
        }
        break;
    }
}

/* ---------------------------------------------------------------------------- stepping */

void game_step(Game *g, uint16_t buttons)
{
    int16_t e;
    int16_t dead = g->tilted || g->state != GS_PLAY;
    PHYS_FLIPPER(&g->w, 0, !dead && (buttons & BTN_LEFT));
    PHYS_FLIPPER(&g->w, 1, !dead && (buttons & BTN_RIGHT));
    PHYS_PLUNGER(&g->w, buttons & BTN_PLUNGER);
    g->w.nev = 0;
    phys_step(&g->w);
    for (e = 0; e < g->w.nev; e++)
        handle_event(g, &g->w.ev[e]);
    g->w.nev = 0;
}

static void end_of_ball(Game *g)
{
    if (g->end_t == 0) {
        int32_t b = 0;
        char s[MSG_LEN];
        if (!g->tilted) {
            b = (5000L * g->ramps + 3000L * g->orbits + 100L * g->spins + 200L * g->bumps) * g->epoch;
            g->bonus = b;
            g->score += b;
        }
        fmt_score(b, s);
        game_message(g, g->tilted ? "TILT" : "BONUS", s, 120, 1);
        g->multiball = 0;
        g->jackpot_lit = 0;
        g->super_t = 0;
        g->turing_t = 0;
        if (g->face != FACE_OFF && !g->turing_lit)
            g->face = FACE_OFF;
    }
    g->end_t++;
    if (g->end_t < 130)
        return;
    g->end_t = 0;
    if (g->ball >= g->balls_per_game) {
        g->state = GS_OVER;
        g->over_t = 0;
        game_message(g, "GAME OVER", "", 240, 1);
        g->sounds |= SND_DRAIN;
        say(g, SAY_GAMEOVER);
        return;
    }
    g->ball++;
    reset_ball_state(g);
    serve_ball(g);
    {
        char t[MSG_LEN] = "BALL 1";
        t[5] = (char)('0' + g->ball);
        game_message(g, t, g->ai_playing ? "AI PLAYING" : "", 100, 0);
    }
}

static int16_t any_flipper_held(const Game *g)
{
    return g->w.flip[0].held || g->w.flip[1].held;
}

void game_tick(Game *g, uint16_t buttons)
{
    int16_t i, nball = 0;
    uint16_t pressed = (uint16_t)(buttons & ~g->prev_buttons);
    g->ticks++;
    g->lamp_blink++;
    if (g->msg_t > 0 && --g->msg_t == 0) {
        g->msg[0][0] = g->msg[1][0] = 0;
        g->msg_big = 0;
    }
    if (g->state == GS_OVER) {
        g->over_t++;
        g->prev_buttons = buttons;
        return;
    }
    if (g->state != GS_PLAY) {
        g->prev_buttons = buttons;
        return;
    }
    /* lane change: either flipper button moves the lit lanes */
    if (!g->tilted && (pressed & (BTN_LEFT | BTN_RIGHT)) && g->lanes && g->lanes != 0x1F) {
        if (pressed & BTN_LEFT)
            g->lanes = (int16_t)(((g->lanes >> 1) | (g->lanes << 4)) & 0x1F);
        else
            g->lanes = (int16_t)(((g->lanes << 1) | (g->lanes >> 4)) & 0x1F);
    }
    if (pressed & (BTN_LEFT | BTN_RIGHT))
        g->sounds |= SND_FLIPPER;
    /* nudge and tilt */
    if (g->nudge_cool > 0)
        g->nudge_cool--;
    if ((pressed & BTN_NUDGE) && !g->nudge_cool && !g->tilted) {
        for (i = 0; i < MAX_BALLS; i++) {
            Ball *b = &g->w.ball[i];
            if (b->state == BALL_FREE && b->x < FX(236)) {
                b->vy -= 65536;                      /* about 120 px/s up ... */
                b->vx += b->x < FX(121) ? 31456 : -31456;   /* ... and 58 px/s to the middle */
                b->sleep = 0;
            }
        }
        g->nudge_cool = 12;
        g->tilt_meter += 38;
        if (g->tilt_meter > 100) {
            g->tilt_warn++;
            g->tilt_meter = 60;
            if (g->tilt_warn >= 3) {
                g->tilted = 1;
                g->w.tilted = 1;
                game_message(g, "TILT", "", 240, 1);
                g->sounds |= SND_TILT;
                say(g, SAY_TILT);
            } else {
                game_message(g, "DANGER", g->tilt_warn == 1 ? "WARNING 1 OF 2" : "WARNING 2 OF 2", 90, 1);
                g->sounds |= SND_TILT;
            }
        }
    }
    if (g->tilt_meter > 0 && (g->ticks & 1))
        g->tilt_meter--;
    /* timers */
    if (g->save_t > 0)
        g->save_t--;
    if (g->skill_t > 0 && --g->skill_t == 0)
        g->skill_lane = -1;
    if (g->super_t > 0 && --g->super_t == 0)
        g->jackpot_lit = g->multiball != 0;
    if (g->turing_t > 0 && --g->turing_t == 0) {
        g->face = FACE_SAD;
        g->face_t = 180;
        g->turing_targets = 0;
        game_message(g, "TURING TEST", "FAILED", 150, 1);
        say(g, SAY_FAILED);
    }
    if (g->face_t > 0 && --g->face_t == 0)
        g->face = g->turing_t ? FACE_SUSPICIOUS : g->turing_lit ? FACE_ON : FACE_OFF;
    if (g->drop_reset_t > 0 && --g->drop_reset_t == 0)
        drops_reset(g);
    if (g->kickback)
        phys_arm(&g->w, g->sh_kickback, 1);
    /* the spinner keeps turning */
    g->spinner_ang = (int16_t)((g->spinner_ang + g->spinner_w / 4) & 0xFF);
    if (g->spinner_w > 0)
        g->spinner_w = (int16_t)(g->spinner_w - g->spinner_w / 24 - 1);
    if (g->spinner_w < 0)
        g->spinner_w = 0;
    /* held balls (sink): eject after the hold */
    for (i = 0; i < MAX_BALLS; i++) {
        Ball *b = &g->w.ball[i];
        if (b->state == BALL_HELD && g->held_t[i] > 0 && --g->held_t[i] == 0) {
            fx vx = -FX(2) - (fx)(rnd(g) % 48000);       /* about 260 px/s down-left */
            fx vy = FX(4) - (fx)(rnd(g) % 32000);
            phys_eject(&g->w, i, vx, vy);
            g->sounds |= SND_KICK;
        }
        if (b->state != BALL_OFF)
            nball++;
        /* ball search: a ball sitting still where nothing holds it gets a shake */
        if (b->state == BALL_FREE && b->still > STEPS_PER_TICK * 150 && !any_flipper_held(g) &&
            b->x < FX(236)) {
            b->vx += (rnd(g) & 1) ? 80000 : -80000;
            b->vy -= 120000;
            b->sleep = 0;
            b->still = 0;
        }
    }
    g->in_play = nball;
    if (g->multiball && nball <= 1) {
        g->multiball = 0;
        g->jackpot_lit = 0;
        g->super_t = 0;
        game_message(g, "MULTIBALL OVER", "", 90, 0);
    }
    if (nball == 0)
        end_of_ball(g);
    /* the bowl's hole is open (catches any ball) while it holds a lit award, otherwise only slow
       balls converge into it */
    g->w.sink_vmax = (g->super_t || (!g->multiball && (g->lock_lit || (g->turing_lit && !g->turing_t))))
                         ? 32767 : 2550;
    /* the attract-mode AI: what to aim for */
    if (g->ai_playing) {
        int16_t want;
        if (g->super_t)
            want = AIM_SINK;
        else if (g->multiball && g->jackpot_lit)
            want = AIM_RAMP;
        else if (g->turing_t)
            want = g->turing_targets != 1 ? AIM_RAMP : AIM_DROP;
        else if (g->lock_lit || g->turing_lit)
            want = AIM_SINK;
        else if (g->drops_down != 7)
            want = (g->ticks / 600) & 1 ? AIM_RAMP : AIM_DROP;
        else
            want = (g->ticks / 600) % 3 == 0 ? AIM_ORBIT : (g->ticks / 600) % 3 == 1 ? AIM_RAMP : AIM_LANES;
        g->ai.want = want;
    }
    /* lamps */
    {
        uint32_t L = 0;
        int16_t blink = (g->lamp_blink >> 3) & 1, fast = (g->lamp_blink >> 2) & 1;
#define ON(l) (L |= 1UL << (l))
        for (i = 0; i < 3; i++) {
            if (g->bump_flash & (1 << i))
                g->flash_t[i] = 6;
            if (g->flash_t[i]) {
                g->flash_t[i]--;
                ON(LAMP_BUMPER0 + i);
            }
        }
        g->bump_flash = 0;
        for (i = 0; i < 5; i++)
            if ((g->lanes & (1 << i)) || (g->skill_lane == i && (!g->launched || g->skill_t) && blink))
                ON(LAMP_LANE0 + i);
        for (i = 0; i < 3; i++)
            if (g->drops_down & (1 << i))
                ON(LAMP_DROPDOWN0 + i);
        for (i = 2; i <= 5; i++)
            if (g->epoch >= i)
                ON(LAMP_EPOCH2 + i - 2);
        if ((g->jackpot_lit && blink) || (g->turing_t && g->turing_targets != 1 && fast))
            ON(LAMP_ARROW_RAMP);
        if (g->turing_t && g->turing_targets != 2 && fast)
            ON(LAMP_ARROW_ORBIT);
        if (g->turing_t && g->turing_targets != 4 && fast)
            ON(LAMP_ARROW_DROP);
        if ((g->lock_lit && blink) || (g->turing_lit && fast) || (g->super_t && fast))
            ON(LAMP_ARROW_SINK);
        if (g->lock_lit)
            ON(LAMP_LOCK);
        if (g->jackpot_lit || (g->super_t && blink))
            ON(LAMP_JACKPOT);
        if (g->kickback)
            ON(LAMP_KICKBACK);
        if (g->save_t > 3 * TICKS_PER_S || (g->save_t > 0 && blink))
            ON(LAMP_AGAIN);
#undef ON
        g->lamps = L;
    }
    g->prev_buttons = buttons;
}

void game_debug_multiball(Game *g)
{
    if (g->state == GS_PLAY && !g->multiball)
        start_multiball(g);
}

int16_t game_lamp(const Game *g, int16_t lamp)
{
    return (int16_t)((g->lamps >> lamp) & 1);
}

uint16_t game_ai_buttons(Game *g)
{
    return ai_tick(&g->ai, &g->w);
}
