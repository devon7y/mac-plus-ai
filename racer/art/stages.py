"""The six stages of an Endurance day: 1-bit pattern choices (pairs of bytes for even/odd rows)."""
W, B = (0x00, 0x00), (0xFF, 0xFF)
D12, D25, D50, D75, D87 = (0x80, 0x08), (0x88, 0x22), (0xAA, 0x55), (0x77, 0xDD), (0x7F, 0xF7)
D06 = (0x80, 0x00)

# Each stage: road slots per phase: (grass, rumble, road, marker); sky above the mountains;
# mountain body / snow-cap patterns; optional fog (rows with k < fog_k are drawn as `fog`).
STAGES = {
    "morning": dict(slots=[(D25, B, W, W), (D50, W, W, B)], sky=W, mtn=B, cap=W, far=D50),
    "snow":    dict(slots=[(W, B, D25, D25), (D06, W, D25, W)], sky=D12, mtn=B, cap=W, far=D25,
                    grip=0.5),
    "dusk":    dict(slots=[(D75, B, D25, D25), (D87, W, D25, W)], sky=D50, mtn=B, cap=D25, far=D75),
    "night":   dict(slots=[(B, W, B, B), (B, B, B, B)], sky=B, mtn=D87, cap=D75, far=B, night=True),
    "fog":     dict(slots=[(D25, B, W, W), (D50, W, W, B)], sky=D50, mtn=D50, cap=D50, far=D50,
                    fog_k=40, fog=D50),
    "dawn":    dict(slots=[(D50, B, W, W), (D75, W, W, B)], sky=D25, mtn=B, cap=W, far=D50),
}
ORDER = ["morning", "snow", "dusk", "night", "fog", "dawn"]


def rep(b):
    return (b << 24) | (b << 16) | (b << 8) | b


def pattern_sets(S):
    """patterns[phase][parity] -> [grass, rumble, road, marker] as longs."""
    return [[[rep(p[par]) for p in S["slots"][ph]] for par in (0, 1)] for ph in (0, 1)]
