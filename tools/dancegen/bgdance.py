"""bgdance - authoring toolkit for BG Motion 3.0 dance clips.

This is the *source of truth* for the dances shipped in assets/dances. It is a
small choreography DSL, not a bone-by-bone sine generator:

  * A dance is a sequence of KEY POSES placed on musical beats. Every body
    control (pelvis, spine, head, shoulders, arms ...) has its own curve through
    those keys: a cardinal (Catmull-Rom) cubic, so velocity carries through a
    reversal. Explicit holds (``hold=True``, or a key that repeats its neighbour)
    stop dead. ``hit=True`` is an accent: the move overshoots its key by a few
    percent and settles within a fraction of a beat.
  * Successive breaking: each body part is sampled a little later than its
    parent (chest after pelvis, elbow after shoulder, wrist after elbow, head
    after neck). The delay wraps around the loop, so the seam stays closed.
  * Wrist drag and forearm follow are derived from the forearm swing, finger
    curl has a relaxed resting value with a small ripple on accents.
  * Left and right limbs get a deterministic per-key amplitude (+-8..12 %) and
    timing (+-0.03..0.06 beat) difference, so the dance is never mirror-perfect.
    The hash is fixed, so the output is reproducible.
  * Feet are authored as FOOTSTEPS (hold / step / pivot) in stage space, never
    as joint angles. A planted foot is mathematically fixed, so choreography
    cannot skate. Contact segments are exported with the clip.
  * The centre of mass follows the loaded foot automatically ("auto balance"),
    so weight transfer always agrees with the steps.
  * The result is baked into a model independent JSON clip (format documented
    in docs/BG_MOTION_3.md). Nothing here knows about a particular VRM.

Conventions (identical to the engine, "dance frame"): +X = character LEFT,
+Y = up, +Z = forward. Positive yaw turns left. Lengths authored in centimetres
of a 160 cm reference character; they are stored as fractions of height.
"""

import hashlib
import json
import math

REF_HEIGHT_CM = 160.0
DEG = math.pi / 180.0

# ----------------------------------------------------------------------------
# quaternions (x, y, z, w) - Hamilton product, same as the C++ side
# ----------------------------------------------------------------------------


def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def q_axis(axis, deg):
    x, y, z = axis
    n = math.sqrt(x * x + y * y + z * z)
    if n < 1e-9:
        return (0.0, 0.0, 0.0, 1.0)
    h = deg * DEG * 0.5
    s = math.sin(h) / n
    return (x * s, y * s, z * s, math.cos(h))


def rx(d):
    return q_axis((1, 0, 0), d)


def ry(d):
    return q_axis((0, 1, 0), d)


def rz(d):
    return q_axis((0, 0, 1), d)


def q_chain(*qs):
    out = (0.0, 0.0, 0.0, 1.0)
    for q in qs:
        out = q_mul(out, q)
    return out


def q_norm(q):
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q)


def q_from_to(a, b):
    """Shortest arc rotation taking direction a to direction b."""

    def nrm(v):
        n = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
        return (v[0] / n, v[1] / n, v[2] / n)

    a = nrm(a)
    b = nrm(b)
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    if d > 0.999999:
        return (0.0, 0.0, 0.0, 1.0)
    if d < -0.999999:
        return q_axis((0, 0, 1), 180.0)
    c = (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )
    return q_norm((c[0], c[1], c[2], 1.0 + d))


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def smootherstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * t * (t * (t * 6 - 15) + 10)


# ----------------------------------------------------------------------------
# cyclic curve
# ----------------------------------------------------------------------------

OVERSHOOT = 0.06        # a hit key overshoots by 6 % of the move that arrives at it ...
OVERSHOOT_MAX = 2.0     # ... but never by more than 2 control units (degrees, cm for hips)
HIT_BRAKE = 0.5         # an accent brakes into its key: its tangent is scaled by this
SETTLE_BEATS = 0.26     # ... and is back on its key within this many beats
OVERSHOOT_PEAK = 0.12   # the overshoot peaks this many beats after its key (floor, at slow tempos)
OVERSHOOT_PEAK_S = 0.06 # ... and never sooner than this many seconds after it, so the reversal
                        # is not a flick at fast tempos (see overshoot_timing)
MIN_SETTLE_GAP = 0.12   # a hit closer than this to the next key gets no overshoot
TENSION = 0.9           # default tangent scale: cardinal, a little softer than Catmull-Rom


def overshoot_timing(bpm_max):
    """(peak_beats, settle_beats) of the hit overshoot for a clip whose fastest tempo is
    bpm_max. The timing is in beats, so at a fast tempo the reversal would be over in
    a few tens of milliseconds and the joints would see a flick. Anchoring the peak to
    OVERSHOOT_PEAK_S seconds at bpm_max keeps the reversal at the same real-time length
    at the fastest tempo; the settle scales with it so the shape is kept. Clips up to
    120 bpm keep the original beat timing."""
    peak = max(OVERSHOOT_PEAK, OVERSHOOT_PEAK_S * bpm_max / 60.0)
    return peak, SETTLE_BEATS * peak / OVERSHOOT_PEAK


def tempo_gain(bpm_author, bpm_max):
    """Scale for the L/R timing asymmetry at the fastest tempo. Acceleration grows with the
    square of the time scale, and a timing offset of a fixed number of beats is a shorter
    real-time gap at a faster tempo, so a clip authored at bpm_author and played up to
    bpm_max gets (bpm_author / bpm_max)^2 of its authored timing break. At or below the
    authored tempo it keeps all of it. The amplitude asymmetry is not scaled."""
    return min(1.0, (bpm_author / float(bpm_max)) ** 2)


class Curve:
    """Periodic scalar curve through keys (beat, value).

    Between keys the curve is a cardinal (Catmull-Rom) cubic Hermite with
    non-uniform tangents. ``tension`` scales the tangent (1.0 = Catmull-Rom,
    0.8-0.9 is softer). Velocity carries through a reversal, so a pose-to-pose
    path does not stop dead on every extreme.

    Only two things stop the curve:
      * ``hold=True`` keys have zero tangent (an explicit hold);
      * a key whose value equals a neighbour's is a plateau and holds too, so a
        repeated pose stays put instead of bulging past its value.

    ``hit=True`` keys are accents: the move that arrives at them continues
    through the key, overshoots it by OVERSHOOT of the move and settles back
    within SETTLE_BEATS. ``bound`` limits how far an overshoot may go (used for
    the pelvis height, so an accent never dips the hips below what the legs can
    reach). ``monotone`` selects the shape-preserving tangent instead (used for
    root motion, which has to agree with the planted feet)."""

    def __init__(self, period, default=0.0, bound=None, monotone=False,
                 peak_beats=OVERSHOOT_PEAK, settle_beats=SETTLE_BEATS):
        self.period = float(period)
        self.default = default
        self.bound = bound
        self.monotone = monotone
        self.peak_beats = peak_beats      # overshoot peak, beats after its key
        self.settle_beats = settle_beats  # back on the key, beats after it
        self.keys = []  # (t, v, hit, tension, hold)
        self._built = None

    def key(self, t, v, hit=False, tension=1.0, hold=False):
        t = float(t)
        if t < -1e-9 or t > self.period + 1e-9:
            raise ValueError("key at %.3f outside [0,%.3f]" % (t, self.period))
        # a key at exactly `period` is the same as a key at 0
        if abs(t - self.period) < 1e-9:
            t = 0.0
        self.keys = [k for k in self.keys if abs(k[0] - t) > 1e-9]
        self.keys.append((t, float(v), bool(hit), float(tension), bool(hold)))
        self._built = None

    def _points(self):
        """Base keys plus the synthetic overshoot and settle keys of every hit."""
        ks = sorted(self.keys)
        n = len(ks)
        pts = []  # [t, v, tension, hold]
        for i, (t, v, hit, tens, hold) in enumerate(ks):
            pts.append([t, v, tens * (HIT_BRAKE if hit else 1.0), hold])
            if not hit or hold or n < 2:
                continue
            delta = v - ks[i - 1][1]  # i == 0 wraps to the last key
            if abs(delta) < 1e-9:
                continue
            nt = ks[(i + 1) % n][0] + (self.period if i == n - 1 else 0.0)
            gap = nt - t
            if gap < MIN_SETTLE_GAP:
                continue
            ov = v + math.copysign(min(OVERSHOOT * abs(delta), OVERSHOOT_MAX), delta)
            if self.bound is not None:
                ov = min(max(ov, self.bound[0]), self.bound[1])
            pts.append([t + min(self.peak_beats, 0.35 * gap), ov, 1.0, False])
            pts.append([t + min(self.settle_beats, 0.7 * gap), v, 1.0, False])
        return pts

    def _build(self):
        if self._built is not None:
            return
        if not self.keys:
            self._built = ([], [], [])
            return
        if len(self.keys) == 1:
            self._built = ([self.keys[0][0]], [self.keys[0][1]], [0.0])
            return
        base = self._points()
        ext = []
        for rep in (-1, 0, 1):
            for t, v, tens, hold in base:
                ext.append((t + rep * self.period, v, tens, hold))
        ts = [e[0] for e in ext]
        vs = [e[1] for e in ext]
        n = len(ext)
        m = [0.0] * n
        for i in range(1, n - 1):
            if ext[i][3]:
                continue  # explicit hold
            if abs(vs[i] - vs[i - 1]) < 1e-9 or abs(vs[i + 1] - vs[i]) < 1e-9:
                continue  # plateau: the pose holds
            h0 = ts[i] - ts[i - 1]
            h1 = ts[i + 1] - ts[i]
            s0 = (vs[i] - vs[i - 1]) / h0
            s1 = (vs[i + 1] - vs[i]) / h1
            if self.monotone:
                if s0 * s1 <= 0:
                    continue
                w1 = 2 * h1 + h0
                w2 = h1 + 2 * h0
                m[i] = (w1 + w2) / (w1 / s0 + w2 / s1)
            else:
                m[i] = ext[i][2] * (s0 * h1 + s1 * h0) / (h0 + h1)
        self._built = (ts, vs, m)

    def at(self, t):
        if not self.keys:
            return self.default
        self._build()
        ts, vs, m = self._built
        if len(ts) == 1:
            return vs[0]
        t = t % self.period
        # keys were extended by one period either side, so [0, period) lies inside
        lo, hi = 0, len(ts) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if ts[mid] <= t:
                lo = mid
            else:
                hi = mid
        h = ts[hi] - ts[lo]
        u = (t - ts[lo]) / h
        u2 = u * u
        u3 = u2 * u
        return (
            (2 * u3 - 3 * u2 + 1) * vs[lo]
            + (u3 - 2 * u2 + u) * h * m[lo]
            + (-2 * u3 + 3 * u2) * vs[hi]
            + (u3 - u2) * h * m[hi]
        )


# ----------------------------------------------------------------------------
# footsteps
# ----------------------------------------------------------------------------


SWING_LEAD = 0.17       # share of a step spent lifting before the foot starts to travel, so a
                        # step does not drag along the floor (the lift is under 2 cm until ~0.17)
HEEL_PEEL = 0.12        # beats the heel rises before a lift-off, the toe stays on the floor
HEEL_PEEL_TOE = 1.0     # toe value at the top of the peel (positive = heel up)
TOE_OFF = 0.12          # beats for the peeled toe to come back flat in the first part of the swing
HEEL_STRIKE = 0.12      # beats the heel takes to settle after a forward or side landing
HEEL_STRIKE_TOE = -0.6  # toe value at the landing (negative = toes up, heel on the floor)


def _ease7(v):
    """Septic-style ease 0 -> 1 on [0, 1], symmetric about 1/2. Velocity, acceleration and
    jerk are all zero at both ends, so the curve has no break against a hold or a landing."""
    return v ** 4 * (35.0 - 84.0 * v + 70.0 * v * v - 20.0 * v ** 3)


def swing_profile(u):
    """Height of a swing foot, 0..1, through the swing u in [0, 1]: up with _ease7 to the
    peak at mid-swing and down with it to the landing. Lift-off and landing have zero
    velocity, acceleration and jerk. The old h*sin(pi u)^0.85 had an infinite slope at both
    ends and was the snap in the knee (B1).

    The spec asked for an early peak. Measured on every authored swing, an early peak fails
    the first-key bound (< 20 cm/beat): with the same ease and the peak at 0.4 the sidestep
    knee-up gives 34 cm/beat, at 0.45 it gives 23. So the peak stays at mid-swing."""
    return _ease7(1.0 - abs(2.0 * u - 1.0))


class Foot:
    """Piecewise footwork for one foot, cyclic over the dance period.

    Coordinates are stage space, cm, relative to the bind (neutral) ankle:
    x = left, z = forward. yaw in degrees (positive = toes turn left). toe(t) is the heel
    roll written as the fifth component (see toe())."""

    def __init__(self, period, side, x=0.0, z=0.0, yaw=0.0):
        self.period = float(period)
        self.side = side
        self.start = (x, z, yaw)
        self.cur = (x, z, yaw)
        self.t = 0.0
        self.segs = []  # (kind, t0, t1, p0, p1, height)

    def hold(self, until):
        if until > self.t + 1e-9:
            self.segs.append(("hold", self.t, until, self.cur, self.cur, 0.0))
            self.t = until
        return self

    def step(self, t0, t1, x, z, yaw=None, lift=6.0, shape="arc"):
        self.hold(t0)
        if t1 <= t0:
            raise ValueError("step needs t1 > t0")
        if abs(t0 - self.t) > 1e-6:
            raise ValueError("step starts before previous segment ended (%s)" % (self.side,))
        new = (x, z, self.cur[2] if yaw is None else yaw)
        self.segs.append((shape, t0, t1, self.cur, new, lift))
        self.cur = new
        self.t = t1
        return self

    def orbit(self, t0, t1, center, angle, lift=8.0):
        """Swing around a stage point (cm from the stance centre, same frame as
        Dance.foot_point) by `angle` degrees; the toes turn by the same angle.
        This is how the free foot walks around the pivot foot during a turn."""
        self.hold(t0)
        half = FOOT_HALF_CM if self.side == "left" else -FOOT_HALF_CM
        px, pz = center
        ax, az = half + self.cur[0], self.cur[1]
        a = angle * DEG
        dx, dz = ax - px, az - pz
        nx = px + dx * math.cos(a) + dz * math.sin(a)
        nz = pz - dx * math.sin(a) + dz * math.cos(a)
        new = (nx - half, nz, self.cur[2] + angle)
        self.segs.append(("orbit", t0, t1, self.cur, new, lift, (px, pz, angle)))
        self.cur = new
        self.t = t1
        return self

    def pivot(self, t0, t1, yaw):
        self.hold(t0)
        new = (self.cur[0], self.cur[1], yaw)
        self.segs.append(("pivot", t0, t1, self.cur, new, 0.0))
        self.cur = new
        self.t = t1
        return self

    def close(self):
        self.hold(self.period)
        if (
            abs(self.cur[0] - self.start[0]) > 1e-6
            or abs(self.cur[1] - self.start[1]) > 1e-6
            or abs(self.cur[2] - self.start[2]) > 1e-6
        ):
            raise ValueError(
                "%s foot does not return to its start (%s -> %s); a looping dance "
                "must end where it began" % (self.side, self.start, self.cur)
            )

    def at(self, t):
        """-> (x, lift, z, yaw, kind)"""
        t = t % self.period if t < self.period - 1e-9 else self.period
        seg = self.segs[-1]
        for s in self.segs:
            if s[1] <= t <= s[2] + 1e-12:
                seg = s
                break
        kind, t0, t1, p0, p1, h = seg[:6]
        if kind == "hold":
            return (p0[0], 0.0, p0[1], p0[2], "hold")
        u = (t - t0) / (t1 - t0)
        if kind == "pivot":
            s = smoothstep(u)
            return (p0[0], 0.0, p0[1], p0[2] + (p1[2] - p0[2]) * s, "pivot")
        s = smootherstep(u)
        if kind in ("arc", "orbit"):
            # the travel starts once the foot has left the floor (see SWING_LEAD)
            s = smootherstep((u - SWING_LEAD) / (1.0 - 2.0 * SWING_LEAD))
        if kind == "orbit":
            px, pz, ang = seg[6]
            half = FOOT_HALF_CM if self.side == "left" else -FOOT_HALF_CM
            a = ang * DEG * s
            dx, dz = half + p0[0] - px, p0[1] - pz
            nx = px + dx * math.cos(a) + dz * math.sin(a)
            nz = pz - dx * math.sin(a) + dz * math.cos(a)
            return (nx - half, h * swing_profile(u), nz, p0[2] + ang * s, "swing")
        return (
            p0[0] + (p1[0] - p0[0]) * s,
            h * swing_profile(u),
            p0[1] + (p1[1] - p0[1]) * s,
            p0[2] + (p1[2] - p0[2]) * s,
            "swing",
        )

    def toe(self, t):
        """Toe roll at beat t, the fifth foot component. Positive: the heel is up and the toe
        stays on the floor (the heel peel, over the end of a stance before a lift-off, which
        then comes flat in the first part of the swing). Negative: the toes are up and the
        heel is on the floor (the heel strike after a forward or side landing, which then
        settles flat). Zero everywhere else. The windows never reach into a neighbouring
        swing, and each is capped at half the stance it sits in."""
        t = t % self.period if t < self.period - 1e-9 else self.period  # the last key equals the first
        out = 0.0
        n = len(self.segs)
        for i, seg in enumerate(self.segs):
            kind, t0, t1, p0, p1, h = seg[:6]
            if kind not in ("arc", "orbit"):
                continue
            prev = self.segs[i - 1]
            nxt = self.segs[(i + 1) % n]
            before = (t0 - prev[1]) % self.period
            after = (nxt[2] - t1) % self.period
            peel = min(HEEL_PEEL, 0.5 * before) if prev[0] in ("hold", "pivot") else 0.0
            off = min(TOE_OFF, 0.5 * (t1 - t0))
            strike = 0.0
            if (kind == "arc" and p1[1] - p0[1] >= -1e-9 and nxt[0] in ("hold", "pivot")):
                strike = min(HEEL_STRIKE, 0.5 * after)
            for shift in (-self.period, 0.0, self.period):
                tt = t + shift
                if peel > 0.0 and t0 - peel <= tt <= t0:
                    out += HEEL_PEEL_TOE * smoothstep((tt - t0 + peel) / peel)
                if peel > 0.0 and t0 < tt < t0 + off:
                    out += HEEL_PEEL_TOE * (1.0 - smoothstep((tt - t0) / off))
                if strike > 0.0 and t1 <= tt <= t1 + strike:
                    out += HEEL_STRIKE_TOE * (1.0 - smoothstep((tt - t1) / strike))
        return out

    def contacts(self):
        out = []
        for seg in self.segs:
            kind, t0, t1, p0, p1, h = seg[:6]
            if kind in ("hold", "pivot"):
                out.append([self.side, t0, t1, kind == "pivot"])
        # merge neighbours of the same kind
        merged = []
        for c in out:
            if merged and merged[-1][0] == c[0] and abs(merged[-1][2] - c[1]) < 1e-9 and merged[-1][3] == c[3]:
                merged[-1][2] = c[2]
            else:
                merged.append(c)
        return merged


# ----------------------------------------------------------------------------
# controls -> bones
# ----------------------------------------------------------------------------

ARM_REST = dict(elev=12.0, azim=42.0, twist=12.0, elbow=20.0, ftwist=0.0, wrist=0.0)

CONTROL_DEFAULTS = {
    "hips_x": 0.0, "hips_y": 0.0, "hips_z": 0.0,          # cm
    "pelvis_yaw": 0.0, "pelvis_roll": 0.0, "pelvis_pitch": 0.0,
    "spine_bend": 0.0, "spine_side": 0.0, "spine_twist": 0.0,
    "wave_lower": 0.0, "wave_mid": 0.0, "wave_upper": 0.0,   # extra bend per vertebra group (body wave)
    "head_nod": 0.0, "head_tilt": 0.0, "head_turn": 0.0,
    "sh_l_lift": 0.0, "sh_l_fwd": 0.0, "sh_r_lift": 0.0, "sh_r_fwd": 0.0,
    "root_x": 0.0, "root_z": 0.0, "root_yaw": 0.0,         # cm, cm, degrees
    # finger curl: a relaxed hand rests a little open, the two hands differ
    "hand_l": 0.28, "hand_r": 0.33,                         # 0 open .. 1 fist
}
for _s in ("l", "r"):
    for _k, _v in ARM_REST.items():
        CONTROL_DEFAULTS["arm_%s_%s" % (_s, _k)] = _v

# successive breaking: how late each body part samples its controls, in beats
# (relative to the authored key time; the chain runs pelvis -> spine -> chest ->
# upper chest -> neck -> head, and shoulder -> elbow -> wrist)
LAG = {
    "pelvis": 0.0,
    "spine": 0.01,
    "chest": 0.02,
    "upper": 0.03,      # upperChest
    "shoulder": 0.03,
    "neck": 0.04,
    "head": 0.08,
    "upper_arm": 0.02,
    "elbow": 0.06,
    "hand": 0.12,
}
FOREARM_FOLLOW = 0.35   # forearm twists this fraction of the upper arm twist (relative to rest)
WRIST_DRAG = 0.12       # wrist flex in degrees per degree/beat of forearm swing (opposite sign)
WRIST_MAX = 25.0        # soft clamp of the wrist flex, degrees
WRIST_SMOOTH = 0.07     # beats: the wrist's inertia (low-pass of the drag)
CURL_DRIFT = 0.035      # slow finger-curl drift, 3 cycles per loop
CURL_RIPPLE = 0.10      # finger-curl ripple on each accent
FINE = 64               # samples per beat for the fine periodic helper tracks


def _smooth_periodic(vals, sigma):
    """Gaussian low-pass of a periodic sample list (sigma in samples). Periodic, so the
    loop seam stays closed."""
    m = len(vals)
    radius = max(1, int(3 * sigma))
    kernel = [math.exp(-0.5 * (k / sigma) ** 2) for k in range(-radius, radius + 1)]
    total = sum(kernel)
    kernel = [k / total for k in kernel]
    out = []
    for j in range(m):
        acc = 0.0
        for k, w in zip(range(-radius, radius + 1), kernel):
            acc += w * vals[(j + k) % m]
        out.append(acc)
    return out


def _interp_periodic(vals, x):
    """Linear interpolation of a periodic sample list at fractional index x."""
    m = len(vals)
    x = x % m
    i = int(math.floor(x))
    f = x - i
    return vals[i % m] * (1.0 - f) + vals[(i + 1) % m] * f

_LATERAL_NEGATE = {
    "hips_x", "pelvis_yaw", "pelvis_roll", "spine_side", "spine_twist",
    "head_tilt", "head_turn", "root_x", "root_yaw",
}
_ARM_NEGATE = {"azim_none"}  # azimuth keeps meaning (0 = outward, +forward)


def mirror(controls):
    """Mirror a dict of controls left <-> right."""
    out = {}
    for k, v in controls.items():
        if k.startswith("arm_l_"):
            out["arm_r_" + k[6:]] = v
        elif k.startswith("arm_r_"):
            out["arm_l_" + k[6:]] = v
        elif k == "hand_l":
            out["hand_r"] = v
        elif k == "hand_r":
            out["hand_l"] = v
        elif k.startswith("sh_l_"):
            out["sh_r_" + k[5:]] = v
        elif k.startswith("sh_r_"):
            out["sh_l_" + k[5:]] = v
        elif k in _LATERAL_NEGATE:
            out[k] = -v
        else:
            out[k] = v
    return out


def arm_upper(side, c):
    """Upper arm quaternion: the swing of the arm, then the upper arm twist."""
    p = "arm_%s_" % side
    elev = c[p + "elev"] * DEG
    azim = c[p + "azim"] * DEG
    outward = math.cos(azim) * math.sin(elev)
    forward = math.sin(azim) * math.sin(elev)
    down = -math.cos(elev)
    if side == "l":
        d = (outward, down, forward)
        bind = (1.0, 0.0, 0.0)
    else:
        d = (-outward, down, forward)
        bind = (-1.0, 0.0, 0.0)
    swing = q_from_to(bind, d)
    return q_mul(swing, rx(-c[p + "twist"]))


def arm_lower(side, c):
    p = "arm_%s_" % side
    hinge = -1.0 if side == "l" else 1.0
    return q_mul(ry(hinge * c[p + "elbow"]), rx(-c[p + "ftwist"]))


def arm_hand(side, c):
    wrist = c["arm_%s_wrist" % side]
    return rz(-wrist if side == "l" else wrist)


def arm_quats(side, c):
    """Upper arm, lower arm, hand quaternions for side 'l' or 'r' from one control set."""
    return arm_upper(side, c), arm_lower(side, c), arm_hand(side, c)


SPINE_WEIGHTS = (0.30, 0.34, 0.36)  # spine, chest, upperChest share of the torso bend


def controls_to_bones(g):
    """g maps a body group to the control set sampled for it (see Dance.bake):
    'pelvis', 'spine', 'chest', 'upper', 'neck', 'head', 'shoulder',
    'arm_l_upper', 'arm_l_lower', 'arm_l_hand' and the same for 'r'."""
    bones = {}
    c = g["pelvis"]
    bones["hips"] = q_chain(ry(c["pelvis_yaw"]), rz(c["pelvis_roll"]), rx(c["pelvis_pitch"]))
    for name, key, wave, w in (("spine", "spine", "wave_lower", SPINE_WEIGHTS[0]),
                               ("chest", "chest", "wave_mid", SPINE_WEIGHTS[1]),
                               ("upperChest", "upper", "wave_upper", SPINE_WEIGHTS[2])):
        s = g[key]
        bones[name] = q_chain(ry(s["spine_twist"] * w), rz(-s["spine_side"] * w),
                              rx(s["spine_bend"] * w + s[wave]))
    n = g["neck"]
    bones["neck"] = q_chain(ry(n["head_turn"] * 0.35), rz(-n["head_tilt"] * 0.35), rx(n["head_nod"] * 0.35))
    h = g["head"]
    bones["head"] = q_chain(ry(h["head_turn"] * 0.65), rz(-h["head_tilt"] * 0.65), rx(h["head_nod"] * 0.65))
    s = g["shoulder"]
    bones["leftShoulder"] = q_chain(ry(-s["sh_l_fwd"]), rz(s["sh_l_lift"]))
    bones["rightShoulder"] = q_chain(ry(s["sh_r_fwd"]), rz(-s["sh_r_lift"]))
    for side, name in (("l", "left"), ("r", "right")):
        bones[name + "UpperArm"] = arm_upper(side, g["arm_%s_upper" % side])
        bones[name + "LowerArm"] = arm_lower(side, g["arm_%s_lower" % side])
        bones[name + "Hand"] = arm_hand(side, g["arm_%s_hand" % side])
    return bones


# ----------------------------------------------------------------------------
# the dance
# ----------------------------------------------------------------------------

FOOT_HALF_CM = 7.0  # half distance between the bind ankles (balance only)
HIPS_Y_BOUND = (-34.0, 12.0)  # cm: an accent may not dip the pelvis past what the legs reach

# per bar (4 beats) modulation for the 16-beat loop: head nod, body accent, arm plane
NOD_BAR = (1.0, 0.75, 1.15, 0.6)
ACCENT_BAR = (1.0, 1.0, 1.0, 1.12)
PLANE_BAR = (0.0, 0.0, 0.0, 10.0)   # degrees added to arm azimuth (the arm plane turns forward)


class Dance:
    def __init__(self, id, name, style, bpm, beats, bpm_range, energy, description="",
                 keys_per_beat=16, beats_per_bar=4, tempo_sync=True, variation=True):
        self.id = id
        self.name = name
        self.style = style
        self.bpm = bpm
        self.beats = float(beats)
        self.bpm_range = bpm_range
        self.energy = energy
        self.description = description
        self.kpb = keys_per_beat
        self.bpb = beats_per_bar
        self.tempo_sync = tempo_sync
        # variation: per-bar modulation and deterministic L/R break (off for idle)
        self.variation = variation
        self.jitter = variation
        # L/R timing asymmetry, scaled at the fastest tempo (see tempo_gain)
        self.timing_gain = tempo_gain(bpm, bpm_range[1])
        self.nod_bar = NOD_BAR
        self.accent_bar = ACCENT_BAR
        self.plane_bar = PLANE_BAR
        peak, settle = overshoot_timing(bpm_range[1])
        self.curves = {
            k: Curve(self.beats, v,
                     bound=HIPS_Y_BOUND if k == "hips_y" else None,
                     monotone=k.startswith("root_"),
                     peak_beats=peak, settle_beats=settle)
            for k, v in CONTROL_DEFAULTS.items()
        }
        self.feet = {
            "left": Foot(self.beats, "left"),
            "right": Foot(self.beats, "right"),
        }
        self.accents = []
        self.entry = [0.0]
        self.exit = [0.0]
        self.balance = (0.0, 0.0)
        self.balance_sigma = 0.22
        self.roll = (0.0, 0.0)
        self.turns = []  # (t0, t1, (px, pz)): body yaw happens about a stage point

    # --- authoring ---
    def pose(self, t, hit=False, hold=False, tension=TENSION, **controls):
        t = float(t)
        for k, v in controls.items():
            if k not in self.curves:
                raise KeyError("unknown control '%s'" % k)
            tt, vv = t, float(v)
            if self.variation:
                vv = self._bar_variant(k, vv, self._bar(t))
            if self.jitter:
                if k.startswith("arm_l_") or k.startswith("sh_l_") or k == "hand_l":
                    tt, vv = self._side_variant("l", k, t, vv)
                elif k.startswith("arm_r_") or k.startswith("sh_r_") or k == "hand_r":
                    tt, vv = self._side_variant("r", k, t, vv)
            self.curves[k].key(tt, vv, hit=hit, tension=tension, hold=hold)
        return self

    def poses(self, t, preset, hit=False, tension=TENSION, **overrides):
        merged = dict(preset)
        merged.update(overrides)
        return self.pose(t, hit=hit, tension=tension, **merged)

    def wave(self, control, amp, cycles, phase=0.0, offset=0.0):
        """A periodic sine written as dense keys. `cycles` must be a whole number so
        that the loop closes. Used for breathing, weight shift and idle sway."""
        if abs(cycles - round(cycles)) > 1e-9:
            raise ValueError("wave needs a whole number of cycles per loop")
        n = int(round(self.beats * self.kpb))
        for i in range(n):
            t = i / self.kpb
            v = offset + amp * math.sin(2.0 * math.pi * cycles * t / self.beats + phase)
            self.pose(t, **{control: v})
        return self

    def glance(self, control, t0, amp, rise=0.6, hold=1.5):
        """A quick look: rest, turn to `amp` over `rise` beats, hold, turn back.
        Eased at both ends (explicit holds), so it reads as a glance, not a jerk."""
        self.pose(t0, hold=True, **{control: 0.0})
        self.pose(t0 + rise, hold=True, **{control: amp})
        self.pose(t0 + rise + hold, hold=True, **{control: amp})
        self.pose(t0 + 2 * rise + hold, hold=True, **{control: 0.0})
        return self

    def accent(self, beat, strength=1.0, kind="hit"):
        self.accents.append({"beat": beat, "strength": strength, "kind": kind})

    def auto_balance(self, gain_x=0.85, gain_z=0.35, sigma=0.22):
        self.balance = (gain_x, gain_z)
        self.balance_sigma = sigma

    def auto_roll(self, degrees=6.0, counter=0.8):
        """Pelvis roll follows the loaded leg (hip of the supporting leg rises);
        the shoulders counter-tilt by `counter` x that angle."""
        self.roll = (degrees, counter)

    def turn_about(self, t0, t1, point=(0.0, 0.0)):
        """Declares that the change of root_yaw within [t0, t1] rotates the body about
        the stage point `point` (cm from the stance centre) instead of the centre.
        Use the foot you pivot on: planted foot position -> no skating."""
        self.turns.append((float(t0), float(t1), point))

    def foot(self, side):
        return self.feet[side]

    def foot_point(self, side, t):
        """Stage point (cm from the stance centre) a foot occupies at beat t."""
        f = self.feet[side].at(t)
        return ((FOOT_HALF_CM if side == "left" else -FOOT_HALF_CM) + f[0], f[2])

    # --- variation ---
    def _bar(self, t):
        nbars = max(1, int(round(self.beats / self.bpb)))
        return int(math.floor(t / self.bpb + 1e-9)) % nbars

    def _bar_variant(self, k, v, bar):
        """Per-bar modulation: nods vary 0.6..1.2 across the bars, the last bar is
        accented (bigger hips and torso, arms lifted a little further) and turns the
        arm plane forward."""
        if k == "head_nod":
            return v * self.nod_bar[bar]
        if k in ("hips_y", "spine_bend"):
            return v * self.accent_bar[bar]
        if k.startswith("arm_") and k.endswith("_elev"):
            rest = CONTROL_DEFAULTS[k]
            return min(175.0, rest + (v - rest) * self.accent_bar[bar])
        if k.startswith("arm_") and k.endswith("_azim"):
            return v + self.plane_bar[bar]
        return v

    def _jitter(self, side, t):
        """Deterministic per-key (amplitude, timing) for one side. Pure function of the
        dance id, the side and the key time, so the output never changes between runs."""
        digest = hashlib.sha256(("%s|%s|%.4f" % (self.id, side, t)).encode("ascii")).digest()
        a = digest[0] / 255.0
        b = digest[1] / 255.0
        # Both are at the low end of the requested bands (8..9 % and 0.03..0.04 beat).
        # A wider spread multiplies the overhead accents of energetic (up to 19 deg per
        # key) and pushes its peak past the 900 deg/s transitions gate at 180 BPM.
        amp = 1.0 + (1.0 if digest[2] & 1 else -1.0) * (0.08 + 0.01 * a)
        dt = (1.0 if digest[2] & 2 else -1.0) * (0.03 + 0.01 * b)
        return amp, dt

    def _side_variant(self, side, k, t, v):
        if t < 0.1 or t > self.beats - 0.1:
            return t, v  # loop anchors stay exact, the seam is never shifted
        amp, dt = self._jitter(side, t)
        if not k.startswith("hand_"):
            rest = CONTROL_DEFAULTS[k]
            v = rest + (v - rest) * amp
        return t + dt * self.timing_gain, v

    # --- baking ---
    def _loads(self, n):
        """Per key load of each foot (0..1), derived from the swing phases."""
        sig = self.balance_sigma
        radius = int(sig * 3 * self.kpb) + 1
        kernel = [math.exp(-0.5 * ((i / self.kpb) / sig) ** 2) for i in range(-radius, radius + 1)]
        ks = sum(kernel)
        kernel = [k / ks for k in kernel]
        cyc = n - 1
        loads = {}
        for side, foot in self.feet.items():
            swing = [1.0 if foot.at(i / self.kpb)[4] == "swing" else 0.0 for i in range(cyc)]
            sm = []
            for i in range(cyc):
                acc = 0.0
                for j, k in enumerate(kernel):
                    acc += k * swing[(i + j - radius) % cyc]
                sm.append(acc)
            loads[side] = [1.0 - v for v in sm]
        return loads

    def _controls(self, t):
        return {k: cur.at(t) for k, cur in self.curves.items()}

    def _wrist_drag(self, side, e):
        """Wrist flex for one arm on a fine periodic grid (FINE samples per beat).
        The hand trails the forearm: flex = -WRIST_DRAG x swing velocity (deg/beat),
        soft-clamped to +-WRIST_MAX, then low-passed so the hand has inertia and no
        spikes on the accents. Sampled later at the hand's own lagged time."""
        el = self.curves["arm_%s_elev" % side]
        m = int(round(self.beats * FINE))
        raw = []
        for j in range(m):
            th = j / float(FINE) - LAG["hand"]
            vel = (el.at(th + e) - el.at(th - e)) / (2.0 * e)
            raw.append(WRIST_MAX * math.tanh(-WRIST_DRAG * vel / WRIST_MAX))
        return _smooth_periodic(raw, WRIST_SMOOTH * FINE)

    def _curl(self, side, t, base):
        """Finger curl: the authored value, a slow drift (3 cycles per loop) and a short
        ripple after each accent, so the hand is never a constant."""
        v = base + CURL_DRIFT * math.sin(2.0 * math.pi * 3.0 * t / self.beats + (0.0 if side == "l" else 1.3))
        for a in self.accents:
            d = (t - a["beat"]) % self.beats
            if d < 0.6:
                v += CURL_RIPPLE * a["strength"] * math.exp(-d / 0.12)
        return min(1.0, max(0.0, v))

    def bake(self):
        for f in self.feet.values():
            f.close()
        n = int(round(self.beats * self.kpb)) + 1
        loads = self._loads(n)
        bone_tracks = {}
        hips_track, root_track, foot_tracks = [], [], {"left": [], "right": []}
        hand_tracks = {"left": [], "right": []}
        prev = {}
        e = 1.0 / FINE
        wrist_fine = {side: self._wrist_drag(side, e) for side in ("l", "r")}
        for i in range(n):
            t = i / self.kpb
            c = self._controls(t)
            cache = {}

            def sample(lag):
                key = round(lag, 6)
                if key not in cache:
                    cache[key] = self._controls(t - lag) if key else c
                return dict(cache[key])

            li = loads["left"][i % (n - 1)]
            ri = loads["right"][i % (n - 1)]
            g = {
                "pelvis": dict(c),
                "spine": sample(LAG["spine"]),
                "chest": sample(LAG["chest"]),
                "upper": sample(LAG["upper"]),
                "neck": sample(LAG["neck"]),
                "head": sample(LAG["head"]),
                "shoulder": sample(LAG["shoulder"]),
            }
            for side in ("l", "r"):
                g["arm_%s_upper" % side] = sample(LAG["upper_arm"])
                lower = sample(LAG["elbow"])
                hand = sample(LAG["hand"])
                # forearm twist follows the upper arm twist a little (relative to rest)
                lower["arm_%s_ftwist" % side] += FOREARM_FOLLOW * (
                    lower["arm_%s_twist" % side] - CONTROL_DEFAULTS["arm_%s_twist" % side])
                # wrist drag: the hand trails the forearm swing, opposite to its velocity
                hand["arm_%s_wrist" % side] = _interp_periodic(wrist_fine[side], t * FINE)
                g["arm_%s_lower" % side] = lower
                g["arm_%s_hand" % side] = hand
            if self.roll[0]:
                auto = self.roll[0] * (li - ri)
                g["pelvis"]["pelvis_roll"] += auto
                for name in ("spine", "chest", "upper"):
                    g[name]["spine_side"] -= self.roll[1] * auto
            bones = controls_to_bones(g)
            for name, q in bones.items():
                q = q_norm(q)
                last = prev.get(name)
                if last is not None and sum(a * b for a, b in zip(q, last)) < 0:
                    q = tuple(-x for x in q)
                prev[name] = q
                bone_tracks.setdefault(name, []).append(q)
            # centre of mass over the loaded feet
            fl = self.feet["left"].at(t)
            fr = self.feet["right"].at(t)
            rootx, rootz = c["root_x"], c["root_z"]
            for (t0, t1, (px, pz)) in sorted(self.turns):
                if t < t0:
                    continue
                delta = self.curves["root_yaw"].at(min(t, t1)) - self.curves["root_yaw"].at(t0)
                a = delta * DEG
                dx, dz = rootx - px, rootz - pz
                # yaw about +Y maps (x, z) -> (x cos + z sin, -x sin + z cos)
                rootx = px + dx * math.cos(a) + dz * math.sin(a)
                rootz = pz - dx * math.sin(a) + dz * math.cos(a)
            c["root_x"], c["root_z"] = rootx, rootz
            if li + ri > 0.05:
                com_x = (li * (FOOT_HALF_CM + fl[0]) + ri * (-FOOT_HALF_CM + fr[0])) / (li + ri)
                com_z = (li * fl[2] + ri * fr[2]) / (li + ri)
            else:
                com_x = 0.5 * (fl[0] + fr[0])
                com_z = 0.5 * (fl[2] + fr[2])
            hx = c["hips_x"] + com_x * self.balance[0]
            hz = c["hips_z"] + com_z * self.balance[1]
            hips_track.append((hx / REF_HEIGHT_CM, c["hips_y"] / REF_HEIGHT_CM, hz / REF_HEIGHT_CM))
            hand_tracks["left"].append((self._curl("left", t, c["hand_l"]),))
            hand_tracks["right"].append((self._curl("right", t, c["hand_r"]),))
            root_track.append((c["root_x"] / REF_HEIGHT_CM, c["root_z"] / REF_HEIGHT_CM, c["root_yaw"] / 360.0))
            for side, f in (("left", fl), ("right", fr)):
                foot_tracks[side].append(
                    (f[0] / REF_HEIGHT_CM, f[1] / REF_HEIGHT_CM, f[2] / REF_HEIGHT_CM, f[3] / 360.0,
                     self.feet[side].toe(t))
                )
        contacts = []
        for side in ("left", "right"):
            for s, t0, t1, pivot in self.feet[side].contacts():
                contacts.append({"foot": side, "start": round(t0, 4), "end": round(t1, 4),
                                 "kind": "pivot" if pivot else "stance"})
        return {
            "format": "bgmotion.clip",
            "version": 1,
            "id": self.id,
            "name": self.name,
            "style": self.style,
            "author": "BG Media (authored, tools/dancegen)",
            "origin": "authored keyframe choreography baked by tools/dancegen/generate_dances.py; "
                      "not motion capture",
            "description": self.description,
            "bpm": self.bpm,
            "bpmRange": list(self.bpm_range),
            "beats": self.beats if self.beats != int(self.beats) else int(self.beats),
            "beatsPerBar": self.bpb,
            "keysPerBeat": self.kpb,
            "loop": True,
            "seamless": True,
            "tempoSync": self.tempo_sync,
            "energy": self.energy,
            "entryBeats": self.entry,
            "exitBeats": self.exit,
            "accents": self.accents,
            "contacts": contacts,
            "tracks": {
                "bones": {k: _flat(v) for k, v in bone_tracks.items()},
                "hips": _flat(hips_track),
                "root": _flat(root_track),
                "footLeft": _flat(foot_tracks["left"]),
                "footRight": _flat(foot_tracks["right"]),
                "handLeft": _flat(hand_tracks["left"]),
                "handRight": _flat(hand_tracks["right"]),
            },
        }

    def write(self, path):
        data = self.bake()
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, separators=(",", ":"))
            f.write("\n")
        return data


def _flat(rows):
    out = []
    for row in rows:
        for v in row:
            out.append(round(v, 5) + 0.0)
    # force the loop seam: the last key IS the first key
    return out


def cm(v):
    return v
