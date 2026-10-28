"""Hip-Hop, Side Step and Energetic choreography. See bgdance.py for the DSL and
dances.py for Idle and Groove."""

from bgdance import Dance
from dances import arms


# ---------------------------------------------------------------------------
# HIP-HOP: bounce, cross-over steps, shoulder / chest isolations, a controlled turn
# ---------------------------------------------------------------------------


def build_hiphop():
    d = Dance("hiphop", "Hip-Hop", "hiphop", bpm=92, beats=16, bpm_range=(66, 128), energy=0.62,
              description="Street style: deep knee bounce, side step with a cross-over, shoulder and "
                          "chest isolations, different arm planes each phrase, a controlled turn "
                          "on the left foot and short freezes on the strong beats.")
    L, R = d.foot("left"), d.foot("right")
    # phrase 1 (0-4): step out left and close
    L.step(0.4, 1.0, 22, 0, 12, lift=10)
    R.step(1.5, 2.0, 16, 0, 8, lift=10)
    # phrase 2 (4-8): cross-over in front, open to the right, close
    L.step(4.4, 5.0, -7, 14, -20, lift=14)
    R.step(5.4, 6.0, -14, -4, -10, lift=12)
    L.step(6.4, 7.0, -14, 0, 0, lift=10)
    # phrase 3 (8-12): step out right, then turn on the left foot, right foot orbits
    R.step(8.4, 9.0, -34, 0, -12, lift=10)
    L.step(9.4, 10.0, -22, 0, -8, lift=10)
    pivot_pt = (7.0 - 22.0, 0.0)
    L.pivot(10.0, 11.6, -48)
    R.orbit(10.0, 11.6, pivot_pt, -40, lift=7)
    # phrase 4 (12-16): turn back, step home
    L.pivot(13.0, 14.6, -8)
    R.orbit(13.0, 14.0, pivot_pt, 40, lift=7)
    R.step(14.5, 15.2, 0, 0, 0, lift=10)
    L.step(15.2, 16.0, 0, 0, 0, lift=10)
    d.pose(0.0, root_yaw=0.0)
    d.pose(10.0, root_yaw=0.0)
    d.pose(11.6, root_yaw=-40.0)
    d.pose(13.0, root_yaw=-40.0)
    d.pose(14.6, root_yaw=0.0)
    d.turn_about(10.0, 11.6, pivot_pt)
    d.turn_about(13.0, 14.6, pivot_pt)
    d.auto_balance(0.7, 0.3)
    d.auto_roll(10.0, 0.9)

    # knee bounce: down on the beat, rebound on the off-beat; freezes on 3/7/11/15
    for k in range(16):
        down, up = -14.0, -7.0
        if 4 <= k < 8:
            down, up = -16.0, -8.0
        if k in (3, 7, 11, 15):
            down, up = -17.0, -17.0   # freeze: stay low
        d.pose(k, hips_y=down)
        d.pose(k + 0.5, hips_y=up)
        d.pose(k, spine_bend=7.0 if k % 2 == 0 else 3.0)
    # chest pop + wave
    for k in range(16):
        d.pose(k, wave_lower=-4.0, wave_mid=-4.0, wave_upper=-2.0)
        d.pose(k + 0.5, wave_lower=3.0, wave_mid=5.0, wave_upper=8.0 if k % 2 else 3.0)
    for k in (3, 7, 11, 15):
        d.pose(k + 0.05, hit=True, wave_upper=-9.0, wave_mid=-7.0)
        d.pose(k + 0.5, hit=True, wave_upper=-9.0, wave_mid=-7.0)
    # shoulder hits on alternating off-beats
    for k in range(16):
        left = (k % 2 == 0)
        d.pose(k + 0.5, sh_l_lift=16.0 if left else 0.0, sh_r_lift=0.0 if left else 16.0,
               sh_l_fwd=6.0 if left else 0.0, sh_r_fwd=0.0 if left else 6.0)
        d.pose(k + 0.95, sh_l_lift=0.0, sh_r_lift=0.0)
    # torso twist against pelvis
    for k in range(0, 16, 2):
        s_ = 1 if (k // 2) % 2 == 0 else -1
        d.pose(k, pelvis_yaw=12 * s_, spine_twist=-18 * s_)
    # head
    for k in range(16):
        d.pose(k + 0.05, head_nod=9.0)
        d.pose(k + 0.55, head_nod=-2.0)
    for k in range(0, 16, 4):
        sgn = 1 if (k // 4) % 2 == 0 else -1
        d.pose(k, head_turn=14.0 * sgn, head_tilt=-8.0 * sgn)
        d.pose(k + 2, head_turn=-8.0 * sgn, head_tilt=6.0 * sgn)

    # arms, a different plane in every phrase
    # phrase 1: right-arm pump (up/forward) against a low left sweep
    d.poses(0.0, arms((34, 55, 10, 55), (62, 70, 0, 100)))
    d.poses(1.0, arms((54, 25, 20, 40), (100, 60, 0, 105)))
    d.poses(2.0, arms((88, 10, 10, 25), (38, 70, 10, 75)))
    d.poses(3.0, arms((120, 45, 10, 75), (44, 60, 0, 85)), hit=True)
    # phrase 2: sweep across the chest, open, hands on chest at the freeze
    d.poses(4.0, arms((92, 5, 10, 12), (40, 55, 10, 80)))
    d.poses(5.0, arms((50, 70, 5, 100), (70, 40, 10, 60)))
    d.poses(6.0, arms((46, 75, 0, 110), (98, 0, 10, 12)))
    d.poses(7.0, arms((64, 72, 0, 125), (64, 72, 0, 125)), hit=True)
    # phrase 3: diagonal reach, follows the turn
    d.poses(8.0, arms((34, -30, 10, 20), (142, 20, 10, 30)))
    d.poses(9.0, arms((50, -20, 10, 30), (150, 10, 10, 20)))
    d.poses(10.5, arms((118, 30, 10, 60), (40, 40, 10, 40)))
    d.poses(11.0, arms((152, 10, 0, 20), (22, 5, 0, 15)), hit=True)
    # phrase 4: swing down and settle, hands to hips, freeze
    d.poses(12.0, arms((70, 35, 10, 50), (96, 45, 10, 50)))
    d.poses(13.0, arms((36, 40, 10, 55), (38, 40, 10, 55)))
    d.poses(14.0, arms((52, 12, 20, 110), (52, 12, 20, 110)))
    d.poses(15.0, arms((52, 12, 20, 110), (52, 12, 20, 110)), hit=True)
    # fists on the freezes, open hands on the sweeps
    for k, (a_, b_) in {0: (0.3, 0.3), 3: (0.9, 0.9), 5: (0.0, 0.1), 7: (0.9, 0.9), 9: (0.1, 0.0),
                        11: (0.0, 0.8), 13: (0.3, 0.3), 15: (0.9, 0.9)}.items():
        d.pose(k, hand_l=a_, hand_r=b_)
    d.accent(0, 1.0, "downbeat")
    d.accent(3, 0.8, "pop")
    d.accent(7, 1.0, "freeze")
    d.accent(11, 1.0, "freeze")
    d.accent(15, 0.9, "freeze")
    d.entry = [0.0, 2.0, 4.0, 6.0, 8.0]
    d.exit = [0.0, 2.0, 4.0, 6.0, 8.0]
    return d


# ---------------------------------------------------------------------------
# SIDE STEP / FOOTWORK: the feet do the dancing
# ---------------------------------------------------------------------------


def build_sidestep():
    d = Dance("sidestep", "Side Step", "sidestep", bpm=108, beats=16, bpm_range=(80, 150), energy=0.55,
              description="Footwork: side-close steps, a cross-front shuffle, knee lifts, a back "
                          "step with a toe tap and heel pivots. A foot lands on almost every beat.")
    L, R = d.foot("left"), d.foot("right")
    # bar 1: side - close - side - close (travel left)
    L.step(0.5, 1.0, 20, 0, 12, lift=11)
    R.step(1.5, 2.0, 20, 0, 12, lift=11)
    L.step(2.5, 3.0, 36, 0, 22, lift=11)
    R.step(3.5, 4.0, 34, 0, 20, lift=11)
    # bar 2: cross in front, open, uncross, close (back through the middle)
    L.step(4.5, 5.0, 8, 16, -20, lift=14)
    R.step(5.5, 6.0, 0, 0, 0, lift=11)
    L.step(6.5, 7.0, 0, 0, 0, lift=11)
    R.step(7.5, 8.0, -16, 0, -14, lift=11)
    # bar 3: knee lifts with a step, then a back step
    L.step(8.4, 9.0, 0, 0, 0, lift=26)           # knee up, same spot
    R.step(9.2, 10.0, -16, 0, -14, lift=3)
    R.step(10.4, 11.0, -34, 0, -24, lift=26)     # right knee up, lands wide
    L.step(11.3, 12.0, -22, 0, -12, lift=12)
    # bar 4: back step, toe twist, toe tap forward, home
    R.step(12.3, 13.0, -26, -14, -6, lift=12)
    L.step(13.2, 14.0, -12, 0, 30, lift=10)
    R.step(14.2, 14.8, -4, 0, -24, lift=10)
    L.step(14.8, 15.3, 0, 12, 0, lift=12)
    R.step(15.3, 16.0, 0, 0, 0, lift=10)
    L.step(15.45, 16.0, 0, 0, 0, lift=6)
    d.auto_balance(0.75, 0.3)
    d.auto_roll(9.0, 0.8)
    for k in range(16):
        d.pose(k, hips_y=-11.0, spine_bend=5.0)
        d.pose(k + 0.5, hips_y=-6.0, spine_bend=2.0)
    # shoulders swing opposite the stepping leg
    for k in range(16):
        left = (k % 2 == 0)
        d.pose(k + 0.1, sh_l_lift=8.0 if left else -3.0, sh_r_lift=-3.0 if left else 8.0,
               sh_l_fwd=6.0 if left else -3.0, sh_r_fwd=-3.0 if left else 6.0)
    # hips drive the step direction, shoulders answer
    for k in range(0, 16, 2):
        s_ = 1 if (k // 2) % 2 == 0 else -1
        d.pose(k, pelvis_yaw=-11 * s_, spine_twist=14 * s_)
    for k in range(16):
        d.pose(k + 0.1, head_nod=5.0)
        d.pose(k + 0.6, head_nod=0.0)
    for k in range(0, 16, 4):
        d.pose(k, head_turn=-10.0, head_tilt=5.0)
        d.pose(k + 2, head_turn=10.0, head_tilt=-5.0)
    for k in range(16):
        d.pose(k + 0.0, wave_lower=-2.0, wave_mid=-2.0, wave_upper=-2.0)
        d.pose(k + 0.5, wave_lower=2.0, wave_mid=3.0, wave_upper=4.0)
    # arms counter-swing the legs like running arms, elbows bent
    swing_f = (40, 70, 0, 95)
    swing_b = (30, -15, 10, 70)
    wide = (66, 8, 10, 50)
    for k in range(0, 8, 2):
        d.poses(k, arms(swing_f, swing_b))
        d.poses(k + 1, arms(swing_b, swing_f))
    d.poses(7.0, arms(wide, wide))
    d.poses(8.0, arms((44, 55, 0, 100), (100, 25, 10, 70)))
    d.poses(9.0, arms((70, 20, 10, 60), (44, 55, 0, 100)))
    d.poses(10.0, arms((100, 25, 10, 70), (130, 20, 10, 40)), hit=True)
    d.poses(11.0, arms((50, 50, 0, 90), (60, 20, 10, 60)))
    d.poses(12.0, arms((34, 60, 0, 95), (74, 5, 10, 45)))
    d.poses(13.0, arms((80, 0, 10, 40), (40, 70, 0, 95)))
    d.poses(14.0, arms(wide, wide))
    d.poses(15.0, arms((46, 30, 10, 70), (46, 30, 10, 70)))
    d.accent(0, 0.9, "downbeat")
    d.accent(8, 1.0, "knee lift")
    d.accent(10, 1.0, "knee lift")
    d.accent(15, 0.8, "toe tap")
    d.entry = [float(b) for b in range(0, 16, 2)]
    d.exit = [float(b) for b in range(0, 16, 2)]
    return d


# ---------------------------------------------------------------------------
# ENERGETIC / DROP: deep wide squats, big arm sweeps, 90 and 180 degree turns, freezes
# ---------------------------------------------------------------------------


def build_energetic():
    d = Dance("energetic", "Energetic", "energetic", bpm=126, beats=16, bpm_range=(92, 176), energy=0.95,
              description="Drop choreography: wide deep squat with overhead sweeps, side lunges, "
                          "a quarter turn and a 180 degree spin that recover to the front, "
                          "sharp freezes between fast moves and a big final pose.")
    L, R = d.foot("left"), d.foot("right")
    # bar 1: jump out wide into a deep squat, hold, snap in
    L.step(0.5, 1.0, 26, 0, 20, lift=14)
    R.step(0.5, 1.0, -26, 0, -20, lift=14)
    L.step(3.5, 4.0, 4, 0, 0, lift=10)
    R.step(3.5, 4.0, -4, 0, 0, lift=10)
    # bar 2: lunge left, lunge right, close
    L.step(4.5, 5.0, 36, 0, 25, lift=12)
    R.step(5.5, 6.0, -36, 0, -25, lift=12)
    L.step(6.5, 7.0, 12, 0, 8, lift=12)
    R.step(7.0, 7.5, -12, 0, -8, lift=12)
    # bar 3: quarter turn left about the left foot (right foot walks round), freeze, back
    pivot_a = (7.0 + 12.0, 0.0)
    L.pivot(8.4, 9.4, 90 + 8)
    R.orbit(8.4, 9.4, pivot_a, 90, lift=12)
    L.pivot(10.6, 11.6, 8)
    R.orbit(10.6, 11.6, pivot_a, -90, lift=12)
    d.pose(8.4, root_yaw=0.0)
    d.pose(9.4, root_yaw=90.0, tension=1.4)
    d.pose(10.6, root_yaw=90.0)
    d.pose(11.6, root_yaw=0.0, tension=1.4)
    d.turn_about(8.4, 9.4, pivot_a)
    d.turn_about(10.6, 11.6, pivot_a)
    # bar 4: 180 degree spin out about the left foot, big pose, spin back
    pivot_b = pivot_a
    L.pivot(12.0, 13.0, 8 + 180)
    R.orbit(12.0, 13.0, pivot_b, 180, lift=14)
    L.pivot(14.0, 15.0, 8)
    R.orbit(14.0, 15.0, pivot_b, -180, lift=14)
    L.step(15.2, 16.0, 0, 0, 0, lift=8)
    R.step(15.4, 16.0, 0, 0, 0, lift=8)
    d.pose(12.0, root_yaw=0.0)
    d.pose(13.0, root_yaw=180.0, tension=1.4)
    d.pose(14.0, root_yaw=180.0)
    d.pose(15.0, root_yaw=0.0, tension=1.4)
    d.turn_about(12.0, 13.0, pivot_b)
    d.turn_about(14.0, 15.0, pivot_b)
    d.auto_balance(0.0, 0.0)
    d.auto_roll(8.0, 0.7)

    # pelvis: deep squat, lunges shift sideways, otherwise recover tall
    d.pose(0, hips_y=-6.0, hips_x=0)
    d.pose(1.0, hips_y=-30.0, hips_x=0, hit=True)
    d.pose(2.0, hips_y=-33.0, hips_x=0, hit=True)
    d.pose(3.0, hips_y=-20.0)
    d.pose(4.0, hips_y=-10.0, hips_x=0)
    d.pose(5.0, hips_y=-30.0, hips_x=15.0)
    d.pose(6.0, hips_y=-30.0, hips_x=-15.0)
    d.pose(7.0, hips_y=-14.0, hips_x=0)
    d.pose(8.0, hips_y=-12.0)
    d.pose(9.4, hips_y=-24.0, hit=True)
    d.pose(10.4, hips_y=-24.0)
    d.pose(11.6, hips_y=-12.0)
    d.pose(12.0, hips_y=-14.0)
    d.pose(13.0, hips_y=-22.0)
    d.pose(14.0, hips_y=-28.0, hit=True)
    d.pose(15.0, hips_y=-10.0)
    d.pose(15.9, hips_y=-6.0)
    # shoulder hits on the drop, the lunges and the freezes
    d.pose(0.0, sh_l_lift=0.0, sh_r_lift=0.0, sh_l_fwd=0.0, sh_r_fwd=0.0)
    d.pose(1.0, sh_l_lift=14.0, sh_r_lift=14.0, hit=True)
    d.pose(2.0, sh_l_lift=-4.0, sh_r_lift=-4.0, hit=True)
    d.pose(3.0, sh_l_lift=0.0, sh_r_lift=0.0)
    d.pose(5.0, sh_l_lift=-8.0, sh_r_lift=14.0, hit=True)
    d.pose(6.0, sh_l_lift=14.0, sh_r_lift=-8.0, hit=True)
    d.pose(7.0, sh_l_lift=0.0, sh_r_lift=0.0)
    d.pose(9.4, sh_l_lift=10.0, sh_r_lift=-4.0, sh_l_fwd=8.0)
    d.pose(11.6, sh_l_lift=0.0, sh_r_lift=0.0, sh_l_fwd=0.0)
    d.pose(14.0, sh_l_lift=-6.0, sh_r_lift=12.0, hit=True)
    d.pose(15.2, sh_l_lift=0.0, sh_r_lift=0.0)
    # torso accents
    d.pose(0, spine_bend=2.0)
    d.pose(1.0, spine_bend=14.0, hit=True)
    d.pose(2.0, spine_bend=-8.0, hit=True)
    d.pose(3.0, spine_bend=6.0)
    d.pose(5.0, spine_side=-16.0, spine_bend=8.0)
    d.pose(6.0, spine_side=16.0, spine_bend=8.0)
    d.pose(7.0, spine_side=0.0, spine_bend=4.0)
    d.pose(9.4, spine_twist=-14.0, spine_bend=10.0)
    d.pose(10.6, spine_twist=-14.0, spine_bend=-6.0)
    d.pose(11.6, spine_twist=0.0, spine_bend=4.0)
    d.pose(13.0, spine_twist=10.0, spine_bend=6.0)
    d.pose(14.0, spine_twist=-12.0, spine_bend=-12.0, spine_side=10.0, hit=True)
    d.pose(15.0, spine_twist=0.0, spine_bend=4.0, spine_side=0.0)
    for k in range(16):
        d.pose(k + 0.0, wave_lower=-3.0, wave_mid=-3.0, wave_upper=-3.0)
        d.pose(k + 0.5, wave_lower=3.0, wave_mid=4.0, wave_upper=6.0)
    d.pose(0, pelvis_yaw=0.0, head_nod=0.0)
    d.pose(2.0, pelvis_yaw=0.0, head_nod=-14.0, head_turn=0.0)
    d.pose(5.0, pelvis_yaw=10.0, head_nod=0.0, head_turn=-20.0, head_tilt=-12.0)
    d.pose(6.0, pelvis_yaw=-10.0, head_nod=0.0, head_turn=20.0, head_tilt=12.0)
    d.pose(9.4, head_turn=24.0, head_nod=6.0)
    d.pose(10.6, head_turn=24.0, head_nod=6.0)
    d.pose(11.6, head_turn=0.0, head_nod=0.0, head_tilt=0.0)
    d.pose(13.0, head_turn=-18.0, head_tilt=8.0)
    d.pose(14.0, head_turn=-24.0, head_tilt=-16.0, head_nod=-10.0)
    d.pose(15.5, head_turn=0.0, head_tilt=0.0, head_nod=2.0)
    d.pose(3.0, head_turn=0.0, head_tilt=0.0)
    d.pose(7.0, head_turn=0.0, head_tilt=0.0)

    # arms: wide, high and committed
    up_wide = (165, 8, 0, 8)
    sweep_l = (100, 5, 5, 6)
    d.poses(0.0, arms((24, 40, 10, 50), (24, 40, 10, 50)))
    d.poses(1.0, arms((110, 15, 5, 70), (110, 15, 5, 70)), hit=True)
    d.poses(2.0, arms(up_wide, up_wide), hit=True)
    d.poses(3.0, arms((70, 25, 5, 90), (70, 25, 5, 90)))
    d.poses(4.0, arms((30, 40, 10, 60), (30, 40, 10, 60)))
    # the lunges are freezes (explicit holds, no velocity carried through): as accents they
    # would sweep the arms through the key at full speed and break the 900 deg/s gate at 180 BPM
    d.poses(5.0, arms((168, 20, 0, 10), (40, -35, 10, 15)), hold=True)
    d.poses(6.0, arms((40, -35, 10, 15), (168, 20, 0, 10)), hold=True)
    d.poses(7.0, arms((50, 50, 0, 100), (50, 50, 0, 100)))
    d.poses(8.0, arms(sweep_l, (60, 10, 5, 30)))
    d.poses(9.4, arms((130, 30, 0, 30), (100, 60, 0, 90)), hit=True)
    d.poses(10.6, arms((130, 30, 0, 30), (100, 60, 0, 90)))
    d.poses(11.6, arms((60, 40, 10, 70), (60, 40, 10, 70)))
    d.poses(12.0, arms((90, 0, 10, 20), (90, 0, 10, 20)))
    d.poses(13.0, arms((150, 10, 0, 20), (150, 10, 0, 20)))
    d.poses(14.0, arms((172, 5, 0, 5), (62, -25, 10, 20)), hit=True)
    d.poses(15.0, arms((70, 30, 10, 60), (60, 30, 10, 60)))
    # hands: open and spread on the sweeps, fists on the freezes
    d.pose(0.0, hand_l=0.3, hand_r=0.3)
    d.pose(1.0, hand_l=0.9, hand_r=0.9)
    d.pose(2.0, hand_l=0.0, hand_r=0.0)
    d.pose(3.5, hand_l=0.4, hand_r=0.4)
    d.pose(5.0, hand_l=0.0, hand_r=0.7)
    d.pose(6.0, hand_l=0.7, hand_r=0.0)
    d.pose(8.0, hand_l=0.2, hand_r=0.2)
    d.pose(9.4, hand_l=0.0, hand_r=0.8)
    d.pose(11.6, hand_l=0.3, hand_r=0.3)
    d.pose(14.0, hand_l=0.0, hand_r=0.9)
    d.pose(15.5, hand_l=0.3, hand_r=0.3)
    d.accent(1, 1.0, "drop squat")
    d.accent(2, 1.0, "overhead")
    d.accent(5, 0.9, "lunge")
    d.accent(6, 0.9, "lunge")
    d.accent(9.4, 1.0, "freeze")
    d.accent(14, 1.0, "final pose")
    d.entry = [0.0, 2.0, 4.0, 6.0, 8.0, 12.0]
    d.exit = [0.0, 2.0, 4.0, 6.0, 8.0, 12.0]
    return d


REGISTRY = [
    ("hiphop", build_hiphop),
    ("sidestep", build_sidestep),
    ("energetic", build_energetic),
]
