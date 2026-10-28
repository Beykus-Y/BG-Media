"""Choreography for the BG Media dance library. See bgdance.py for the DSL."""

from bgdance import Dance, mirror

# ---------------------------------------------------------------------------
# arm poses: (elevation from straight-down, azimuth 0=outward 90=forward,
#             upper-arm twist, elbow flexion)
# ---------------------------------------------------------------------------


def arm(side, elev, azim, twist=10.0, elbow=20.0, ftwist=0.0, wrist=0.0):
    p = "arm_%s_" % side
    return {p + "elev": elev, p + "azim": azim, p + "twist": twist, p + "elbow": elbow,
            p + "ftwist": ftwist, p + "wrist": wrist}


def arms(left, right):
    out = {}
    out.update(arm("l", *left))
    out.update(arm("r", *right))
    return out


REST = (16, 25, 10, 18)


# ---------------------------------------------------------------------------
# IDLE
# ---------------------------------------------------------------------------


def build_idle():
    # One loop is 28 s (1 beat = 1 s, wall clock). Every life sign is a whole number of
    # cycles per loop, so the loop closes, but the periods do not nest:
    #   breathing 7 cycles (4.0 s), weight shift 3 cycles (9.3 s), sway 5 cycles (5.6 s).
    # Head glances are not periodic: four looks, unevenly spaced, each one eased.
    d = Dance("idle", "Idle", "idle", bpm=60, beats=28, bpm_range=(30, 120), energy=0.0,
              description="Standing idle loop used before, between and after dances: breathing "
                          "every 4 s, a slow weight shift between the feet, a few quiet head "
                          "glances. Runs on wall-clock time (1 beat = 1 s), not on the music.",
              keys_per_beat=16, tempo_sync=False, variation=False)
    d.poses(0, arms((18, 28, 10, 20), (18, 28, 10, 20)))
    # breathing: chest pitch ~2.5 deg, every part of the chest a little later than the one below
    d.wave("wave_lower", 0.6, 7, phase=0.0)
    d.wave("wave_mid", 2.5, 7, phase=0.5)
    d.wave("wave_upper", 1.5, 7, phase=1.0)
    d.wave("hips_y", 0.4, 7, phase=0.5, offset=-1.4)   # soft knees, as before: the legs have reach
    # slight shoulder lift on the breath, the two sides not quite together
    d.wave("sh_l_lift", 1.2, 7, phase=1.3)
    d.wave("sh_r_lift", 1.2, 7, phase=1.5)
    # weight shift between the feet: hips sway sideways, pelvis rolls, arms follow lightly
    d.wave("hips_x", 0.9, 3)
    d.wave("pelvis_roll", 1.0, 3, phase=0.4)
    d.wave("arm_l_elbow", 1.5, 3, offset=20.0)
    d.wave("arm_r_elbow", 1.5, 3, phase=3.14, offset=20.0)
    d.wave("arm_l_elev", 1.0, 5, offset=18.0)
    d.wave("arm_r_elev", 1.0, 5, phase=2.0, offset=18.0)
    # head: slow pitch (about 2 deg) and a small tilt, plus glances of about 4-5 deg of yaw.
    # head yaw = 0.65 x head_turn (the neck takes the rest), so the controls are yaw / 0.65
    d.wave("head_nod", 3.0, 3, phase=0.8)
    d.wave("head_tilt", 1.8, 5, phase=0.2)
    d.glance("head_turn", 2.0, 4.5 / 0.65)
    d.glance("head_turn", 9.0, -4.0 / 0.65)
    d.glance("head_turn", 15.5, 3.5 / 0.65)
    d.glance("head_turn", 21.5, -5.0 / 0.65)
    d.entry = [0.0]
    d.exit = [0.0]
    d.feet["left"].hold(28)
    d.feet["right"].hold(28)
    return d


# ---------------------------------------------------------------------------
# GROOVE: relaxed side-to-side step-close with hip / shoulder counter motion
# ---------------------------------------------------------------------------


def build_groove():
    d = Dance("groove", "Groove", "groove", bpm=96, beats=16, bpm_range=(68, 128), energy=0.3,
              description="Relaxed step-close groove: weight travels left and right, pelvis and "
                          "shoulders counter-rotate, a soft wave runs through the spine.")
    D = 24.0  # side step size (cm)
    L, R = d.foot("left"), d.foot("right")
    L.start = (D, 0.0, 10.0)
    L.cur = (D, 0.0, 10.0)
    R.start = (0.0, 0.0, 0.0)
    R.cur = (0.0, 0.0, 0.0)
    # lead / close pattern, landings on the even beats
    R.step(1.35, 2.0, D, 0, 10, lift=8)
    R.step(3.35, 4.0, 0, 0, 0, lift=8)
    R.step(7.35, 8.0, -D, 0, -10, lift=8)
    R.step(13.35, 14.0, 0, 0, 0, lift=8)
    L.step(5.35, 6.0, 0, 0, 0, lift=8)
    L.step(9.35, 10.0, -D, 0, -10, lift=8)
    L.step(11.35, 12.0, 0, 0, 0, lift=8)
    L.step(15.35, 16.0, D, 0, 10, lift=8)
    d.auto_balance(0.8, 0.3)
    d.auto_roll(9.0, 0.9)

    # pelvis bounce: down on every beat, up on the off-beat
    for k in range(16):
        amp = (-9.0, -4.0)
        if 8 <= k < 12:
            amp = (-12.0, -4.0)
        if 12 <= k:
            amp = (-9.0, -5.0)
        d.pose(k, hips_y=amp[0], spine_bend=4.0)
        d.pose(k + 0.5, hips_y=amp[1], spine_bend=2.0)

    # pelvis and chest turn against each other every two beats
    for k in range(0, 16, 2):
        s = 1 if (k // 2) % 2 == 0 else -1
        d.pose(k, pelvis_yaw=8 * s, spine_twist=-10 * s, pelvis_roll=0)
    # body wave: each vertebra group lags the one below
    for k in range(16):
        d.pose(k + 0.00, wave_lower=-3.0)
        d.pose(k + 0.50, wave_lower=4.0)
    for k in range(16):
        d.pose(k + 0.18, wave_mid=-3.0)
        d.pose(k + 0.68, wave_mid=4.0)
    for k in range(16):
        d.pose(k + 0.36, wave_upper=-3.0)
        d.pose(k + 0.86, wave_upper=4.0)
    # shoulders answer the pelvis: the shoulder on the loaded side drops, the other lifts
    for k in range(16):
        left_high = (k % 2 == 0)
        d.pose(k + 0.15, sh_l_lift=7.0 if left_high else -3.0, sh_r_lift=-3.0 if left_high else 7.0,
               sh_l_fwd=4.0 if left_high else -2.0, sh_r_fwd=-2.0 if left_high else 4.0)
    # rhythmic head: nod on each beat, tilt follows the lead
    for k in range(16):
        d.pose(k + 0.08, head_nod=7.0)
        d.pose(k + 0.58, head_nod=-1.0)
    for k in range(0, 16, 4):
        d.pose(k, head_tilt=-6.0, head_turn=6.0)
        d.pose(k + 2, head_tilt=6.0, head_turn=-6.0)

    # arms: left lead -> right lead -> right lead -> left lead
    open_l = (46, 12, 20, 32)
    open_r = open_l
    fold = (30, 72, 0, 88)
    swing = (26, 45, 8, 55)
    d.poses(0.0, arms(open_l, fold))
    d.poses(1.0, arms((36, 25, 15, 40), (34, 60, 5, 80)))
    d.poses(2.0, arms(swing, swing))
    d.poses(3.0, arms((30, 52, 10, 70), (40, 30, 15, 45)))
    d.poses(4.0, arms(fold, open_r))
    d.poses(5.0, arms((34, 60, 5, 80), (36, 25, 15, 40)))
    d.poses(6.0, arms(swing, swing))
    d.poses(7.0, arms((40, 30, 15, 45), (30, 52, 10, 70)))
    d.poses(8.0, arms(fold, (62, 28, 5, 70)))
    d.poses(10.0, arms(swing, swing))
    d.poses(12.0, arms((62, 28, 5, 70), fold))
    d.poses(14.0, arms(swing, swing))
    d.poses(15.0, arms((30, 40, 10, 50), (30, 40, 10, 50)))
    d.accent(0, 1.0, "downbeat")
    d.accent(4, 0.8, "bar")
    d.accent(8, 0.9, "bar")
    d.accent(12, 0.8, "bar")
    d.entry = [float(b) for b in range(0, 16, 2)]
    d.exit = [float(b) for b in range(0, 16, 2)]
    return d


REGISTRY = [
    ("idle", build_idle),
    ("groove", build_groove),
]
