"""Player movement for Blockworld: walking, collision, gravity, jumping and auto-jump.

Fixed point: positions in 8.8 blocks, velocities in 1/256 block per tick, 20 ticks per second
(the Mac runs as many ticks per frame as the elapsed Ticks call for). x, z = centre of the feet,
y = bottom of the feet. Integer arithmetic only, so the C port behaves identically.
"""
import world as W

HW = 77            # half width 0.3 blocks
HT = 461           # height 1.8 blocks
EYE = 415          # eye 1.62 blocks above the feet
WALK = 55          # 4.3 blocks/s
GRAV = 20          # 0.08 blocks/tick^2
JUMP = 120         # 0.47 blocks/tick: jumps 1.17 blocks
VMAX = 200         # terminal fall speed

SIN = None         # filled from render (Q14, 1024 per turn)
COS = None


def solid_at(b, x, z, y):
    """Is block (x, z, y) solid? Outside the 64 x 64 square counts as solid (an invisible wall)
    except above the world; below y = 0 is solid."""
    if y < 0:
        return True
    if y >= W.WY:
        return False
    if not (0 <= x < W.WX and 0 <= z < W.WX):
        return True
    # a fence also blocks the half block above it (nothing jumps over)
    return W.SOLID[b[W.idx(x, z, y)]] or (y > 0 and b[W.idx(x, z, y - 1)] == W.FENCE)


class Player:
    def __init__(self, x, y, z, yaw=0):
        self.x, self.y, self.z = x, y, z
        self.vy = 0
        self.yaw = yaw & 1023
        self.pitch = 0
        self.ground = False
        self.autojumps = 0

    def box_hits(self, b, x, y, z):
        """Does the player's box at (x, y, z) overlap a solid block?"""
        x0, x1 = (x - HW) >> 8, (x + HW - 1) >> 8
        z0, z1 = (z - HW) >> 8, (z + HW - 1) >> 8
        y0, y1 = y >> 8, (y + HT - 1) >> 8
        for bx in range(x0, x1 + 1):
            for bz in range(z0, z1 + 1):
                for by in range(y0, y1 + 1):
                    if solid_at(b, bx, bz, by):
                        return True
        return False

    def tick(self, b, fwd, strafe, jump, dyaw=0, dpitch=0):
        """One 1/20 s step. fwd, strafe in -1..1 (W/S, D/A), jump = Space held."""
        self.yaw = (self.yaw + dyaw) & 1023
        self.pitch = max(-200, min(200, self.pitch + dpitch))
        c, s = COS[self.yaw], SIN[self.yaw]
        # forward is (c, s); right (strafe +) is (-s, c)
        mx = (fwd * c - strafe * s)
        mz = (fwd * s + strafe * c)
        if fwd and strafe:                      # diagonal: scale by 1/sqrt(2) (181/256)
            mx = (mx * 181) >> 8
            mz = (mz * 181) >> 8
        dx = (mx * WALK) >> 14
        dz = (mz * WALK) >> 14
        blocked = False
        if dx:
            nx = self.x + dx
            if self.box_hits(b, nx, self.y, self.z):
                blocked = True
                nx = (((nx + HW - 1) >> 8) << 8) - HW if dx > 0 else ((((nx - HW) >> 8) + 1) << 8) + HW
                if self.box_hits(b, nx, self.y, self.z):
                    nx = self.x
            self.x = nx
        if dz:
            nz = self.z + dz
            if self.box_hits(b, self.x, self.y, nz):
                blocked = True
                nz = (((nz + HW - 1) >> 8) << 8) - HW if dz > 0 else ((((nz - HW) >> 8) + 1) << 8) + HW
                if self.box_hits(b, self.x, self.y, nz):
                    nz = self.z
            self.z = nz
        # auto-jump: walking into a one-block step with room above it
        if blocked and self.ground and (dx or dz):
            if not self.box_hits(b, self.x + dx, self.y + 256 + 8, self.z + dz):
                jump = True
                self.autojumps += 1
        if jump and self.ground:
            self.vy = JUMP
            self.ground = False
        # gravity, then vertical movement (jump apex 100+80+60+40+20 = 300: 1.17 blocks)
        self.vy -= GRAV
        if self.vy < -VMAX:
            self.vy = -VMAX
        ny = self.y + self.vy
        if self.box_hits(b, self.x, ny, self.z):
            if self.vy < 0:
                ny = ((self.y >> 8) << 8)               # land on the block below
                while self.box_hits(b, self.x, ny, self.z):
                    ny += 256
                self.ground = True
            else:
                ny = self.y
            self.vy = 0
        else:
            self.ground = False
        self.y = ny

    def cam(self, render):
        return render.Cam(self.x, self.y + EYE, self.z, self.yaw, self.pitch)
