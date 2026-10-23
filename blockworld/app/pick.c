/* Blockworld: the block under the crosshair (a 3D DDA along the centre ray) and its outline. */
#include "pick.h"
#include "tables.h"

#define F 256                   /* focal length in pixels (render.c) */
#define BIG 0x00400000L         /* (frac * BIG must fit 31 bits) */

void pick_ray(const RCam *cam, short aim, Pick *r, int (*solid)(int x, int y, int z))
{
    long pos[3], dir[3], dd[3], sd[3], ca = bw_sin[(aim + 256) & 1023];
    short cell[3], step[3], prev[3], a, n;
    static const short facen[3][2] = {{F_XP, F_XN}, {F_YP, F_YN}, {F_ZP, F_ZN}};
    pos[0] = cam->x; pos[1] = cam->y; pos[2] = cam->z;
    dir[0] = (bw_sin[(cam->yaw + 256) & 1023] * ca) >> 14;      /* a unit vector (Q14) */
    dir[2] = (bw_sin[cam->yaw & 1023] * ca) >> 14;
    dir[1] = bw_sin[aim & 1023];
    r->hit = 0;
    r->t = PICK_REACH;
    for (a = 0; a < 3; a++) {
        long ad = dir[a] < 0 ? -dir[a] : dir[a], frac = pos[a] & 255;
        cell[a] = (short)(pos[a] >> 8);
        dd[a] = ad < 16 ? BIG : (1L << 22) / ad;  /* 8.8 distance along the ray per block */
        if (dir[a] > 0) { step[a] = 1; sd[a] = ((256 - frac) * dd[a]) >> 8; }
        else { step[a] = -1; sd[a] = (frac * dd[a]) >> 8; }
    }
    if (solid(cell[0], cell[1], cell[2])) return;  /* the eye inside a block */
    for (n = 0; n < 40; n++) {
        a = sd[0] < sd[1] ? (sd[0] < sd[2] ? 0 : 2) : (sd[1] < sd[2] ? 1 : 2);
        if (sd[a] > PICK_REACH) return;
        prev[0] = cell[0]; prev[1] = cell[1]; prev[2] = cell[2];
        cell[a] = (short)(cell[a] + step[a]);
        if (solid(cell[0], cell[1], cell[2])) {
            r->hit = 1;
            r->t = sd[a];
            r->x = cell[0]; r->y = cell[1]; r->z = cell[2];
            r->face = facen[a][step[a] > 0];
            r->ax = prev[0]; r->ay = prev[1]; r->az = prev[2];
            return;
        }
        sd[a] += dd[a];
    }
}

/* ---- the outline ---------------------------------------------------------------------- */

static unsigned char *ob;
static short ow, oh;

static void xpix(short x, short y)
{
    short b = x >> 3;
    ob[(long)(b >> 1) * 2 * oh + 2 * y + (b & 1)] ^= (unsigned char)(0x80 >> (x & 7));
}

static short outcode(long x, long y)
{
    return (short)((x < 0) | ((x >= ow) << 1) | ((y < 0) << 2) | ((y >= oh) << 3));
}

/* a line clipped to the frame (Cohen-Sutherland), drawn with Bresenham, end point excluded */
static void line(long x0, long y0, long x1, long y1)
{
    short c0 = outcode(x0, y0), c1 = outcode(x1, y1);
    long dx, dy, sx, sy, err, k;
    while (c0 | c1) {
        long x, y;
        short c;
        if (c0 & c1) return;
        c = c0 ? c0 : c1;
        if (c & 8) { x = x0 + (x1 - x0) * (oh - 1 - y0) / (y1 - y0); y = oh - 1; }
        else if (c & 4) { x = x0 + (x1 - x0) * (0 - y0) / (y1 - y0); y = 0; }
        else if (c & 2) { y = y0 + (y1 - y0) * (ow - 1 - x0) / (x1 - x0); x = ow - 1; }
        else { y = y0 + (y1 - y0) * (0 - x0) / (x1 - x0); x = 0; }
        if (c == c0) { x0 = x; y0 = y; c0 = outcode(x0, y0); }
        else { x1 = x; y1 = y; c1 = outcode(x1, y1); }
    }
    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;
    for (k = 0; k < 800; k++) {
        long e2;
        if (x0 == x1 && y0 == y1) break;
        xpix((short)x0, (short)y0);
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

#define DNEAR 24                /* the near plane: about 0.1 block */

void pick_outline(const RCam *cam, const Pick *r, unsigned char *buf, short w, short h)
{
    long c = bw_sin[(cam->yaw + 256) & 1023], s = bw_sin[cam->yaw & 1023];
    long P[4][3], V[4][3], Q[8][3], X[8], Y[8];
    short i, n = 0, h0 = (short)(h / 2 + cam->pitch);
    long bx = (long)r->x << 8, by = (long)r->y << 8, bz = (long)r->z << 8;
    static const short sq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    if (!r->hit) return;
    ob = buf; ow = w; oh = h;
    for (i = 0; i < 4; i++) {                         /* the face's corners (8.8 world) */
        long u = (long)sq[i][0] << 8, v = (long)sq[i][1] << 8;
        switch (r->face) {
        case F_XN: P[i][0] = bx; P[i][1] = by + u; P[i][2] = bz + v; break;
        case F_XP: P[i][0] = bx + 256; P[i][1] = by + u; P[i][2] = bz + v; break;
        case F_YN: P[i][0] = bx + u; P[i][1] = by; P[i][2] = bz + v; break;
        case F_YP: P[i][0] = bx + u; P[i][1] = by + 256; P[i][2] = bz + v; break;
        case F_ZN: P[i][0] = bx + u; P[i][1] = by + v; P[i][2] = bz; break;
        default:   P[i][0] = bx + u; P[i][1] = by + v; P[i][2] = bz + 256; break;
        }
    }
    for (i = 0; i < 4; i++) {                         /* view space: depth, right, up (8.8) */
        long rx = P[i][0] - cam->x, ry = P[i][1] - cam->y, rz = P[i][2] - cam->z;
        V[i][0] = (rx * c + rz * s) >> 14;
        V[i][1] = (rz * c - rx * s) >> 14;
        V[i][2] = ry;
    }
    for (i = 0; i < 4; i++) {                         /* clip the quad at the near plane */
        const long *p = V[i], *q = V[(i + 1) & 3];
        if (p[0] >= DNEAR) { Q[n][0] = p[0]; Q[n][1] = p[1]; Q[n][2] = p[2]; n++; }
        if ((p[0] >= DNEAR) != (q[0] >= DNEAR)) {
            long t = ((DNEAR - p[0]) << 8) / (q[0] - p[0]);
            Q[n][0] = DNEAR;
            Q[n][1] = p[1] + (((q[1] - p[1]) * t) >> 8);
            Q[n][2] = p[2] + (((q[2] - p[2]) * t) >> 8);
            n++;
        }
    }
    if (n < 2) return;
    for (i = 0; i < n; i++) {
        X[i] = w / 2 + Q[i][1] * F / Q[i][0];
        Y[i] = h0 - Q[i][2] * F / Q[i][0];
    }
    for (i = 0; i < n; i++)
        line(X[i], Y[i], X[(i + 1) % n], Y[(i + 1) % n]);
}
