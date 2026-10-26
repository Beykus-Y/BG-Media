"""bgdance - authoring toolkit for BG Motion 3.0 dance clips.

This is the *source of truth* for the dances shipped in assets/dances. It is a
small choreography DSL, not a bone-by-bone sine generator:

  * A dance is a sequence of KEY POSES placed on musical beats. Every body
    control (pelvis, spine, head, shoulders, arms ...) has its own curve through
    those keys (shape-preserving cubic, so a hold stays a hold and a pose is
    reached with natural ease-in / ease-out; ``hit=True`` forces a full stop).
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
# cyclic shape-preserving curve
# ----------------------------------------------------------------------------


class Curve:
    """Periodic scalar curve through keys (beat, value).

    Between keys the curve is a monotone cubic Hermite (Fritsch-Carlson), so it
    never overshoots a key on its own: a hold stays a hold, accents land
    exactly on their key. Authors add explicit anticipation / overshoot keys.
    ``hit`` keys have zero velocity (a full stop). ``tension`` > 1 makes the
    curve arrive faster (snappier), < 1 softer."""

    def __init__(self, period, default=0.0):
        self.period = float(period)
        self.default = default
        self.keys = []  # (t, v, hit, tension)
        self._built = None

    def key(self, t, v, hit=False, tension=1.0):
        t = float(t)
        if t < -1e-9 or t > self.period + 1e-9:
            raise ValueError("key at %.3f outside [0,%.3f]" % (t, self.period))
        # a key at exactly `period` is the same as a key at 0
        if abs(t - self.period) < 1e-9:
            t = 0.0
        self.keys = [k for k in self.keys if abs(k[0] - t) > 1e-9]
        self.keys.append((t, float(v), bool(hit), float(tension)))
        self._built = None

    def _build(self):
        if self._built is not None:
            return
        if not self.keys:
            self._built = ([], [], [])
            return
        ks = sorted(self.keys)
        if len(ks) == 1:
            self._built = ([ks[0][0]], [ks[0][1]], [0.0])
            return
        ext = []
        for rep in (-1, 0, 1):
            for t, v, hit, tens in ks:
                ext.append((t + rep * self.period, v, hit, tens))
        ts = [e[0] for e in ext]
        vs = [e[1] for e in ext]
        n = len(ext)
        h = [ts[i + 1] - ts[i] for i in range(n - 1)]
        d = [(vs[i + 1] - vs[i]) / h[i] for i in range(n - 1)]
        m = [0.0] * n
        for i in range(1, n - 1):
            if ext[i][2]:
                m[i] = 0.0
                continue
            if d[i - 1] * d[i] <= 0:
                m[i] = 0.0
            else:
                w1 = 2 * h[i] + h[i - 1]
                w2 = h[i] + 2 * h[i - 1]
                m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
                m[i] *= ext[i][3]
        m[0] = d[0]
        m[-1] = d[-1]
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


class Foot:
    """Piecewise footwork for one foot, cyclic over the dance period.

    Coordinates are stage space, cm, relative to the bind (neutral) ankle:
    x = left, z = forward. yaw in degrees (positive = toes turn left)."""

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
        if kind == "orbit":
            px, pz, ang = seg[6]
            half = FOOT_HALF_CM if self.side == "left" else -FOOT_HALF_CM
            a = ang * DEG * s
            dx, dz = half + p0[0] - px, p0[1] - pz
            nx = px + dx * math.cos(a) + dz * math.sin(a)
            nz = pz - dx * math.sin(a) + dz * math.cos(a)
            return (nx - half, h * math.sin(math.pi * u), nz, p0[2] + ang * s, "swing")
        lift = h * math.sin(math.pi * u) ** 0.85 if kind == "arc" else h * math.sin(math.pi * u)
        return (
            p0[0] + (p1[0] - p0[0]) * s,
            lift,
            p0[1] + (p1[1] - p0[1]) * s,
            p0[2] + (p1[2] - p0[2]) * s,
            "swing",
        )

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
    "hand_l": 0.3, "hand_r": 0.3,                           # finger curl 0 open .. 1 fist
}
for _s in ("l", "r"):
    for _k, _v in ARM_REST.items():
        CONTROL_DEFAULTS["arm_%s_%s" % (_s, _k)] = _v

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


def arm_quats(side, c):
    """Upper arm, lower arm, hand quaternions for side 'l' or 'r'."""
    p = "arm_%s_" % side
    elev = c[p + "elev"] * DEG
    azim = c[p + "azim"] * DEG
    outward = math.cos(azim) * math.sin(elev)
    forward = math.sin(azim) * math.sin(elev)
    down = -math.cos(elev)
    if side == "l":
        d = (outward, down, forward)
        bind = (1.0, 0.0, 0.0)
        hinge = -1.0
    else:
        d = (-outward, down, forward)
        bind = (-1.0, 0.0, 0.0)
        hinge = 1.0
    swing = q_from_to(bind, d)
    upper = q_mul(swing, rx(-c[p + "twist"]))
    lower = q_mul(ry(hinge * c[p + "elbow"]), rx(-c[p + "ftwist"]))
    wrist = c[p + "wrist"]
    hand = rz(-wrist if side == "l" else wrist)
    return upper, lower, hand


def spine_chain(bend, side, twist, waves=(0.0, 0.0, 0.0)):
    weights = (("spine", 0.30), ("chest", 0.34), ("upperChest", 0.36))
    out = {}
    for (name, w), wave in zip(weights, waves):
        out[name] = q_chain(ry(twist * w), rz(-side * w), rx(bend * w + wave))
    return out


def controls_to_bones(c):
    bones = {}
    bones["hips"] = q_chain(ry(c["pelvis_yaw"]), rz(c["pelvis_roll"]), rx(c["pelvis_pitch"]))
    bones.update(spine_chain(c["spine_bend"], c["spine_side"], c["spine_twist"],
                             (c["wave_lower"], c["wave_mid"], c["wave_upper"])))
    nod, tilt, turn = c["head_nod"], c["head_tilt"], c["head_turn"]
    bones["neck"] = q_chain(ry(turn * 0.35), rz(-tilt * 0.35), rx(nod * 0.35))
    bones["head"] = q_chain(ry(turn * 0.65), rz(-tilt * 0.65), rx(nod * 0.65))
    bones["leftShoulder"] = q_chain(ry(-c["sh_l_fwd"]), rz(c["sh_l_lift"]))
    bones["rightShoulder"] = q_chain(ry(c["sh_r_fwd"]), rz(-c["sh_r_lift"]))
    for side, name in (("l", "left"), ("r", "right")):
        upper, lower, hand = arm_quats(side, c)
        bones[name + "UpperArm"] = upper
        bones[name + "LowerArm"] = lower
        bones[name + "Hand"] = hand
    return bones


# ----------------------------------------------------------------------------
# the dance
# ----------------------------------------------------------------------------

FOOT_HALF_CM = 7.0  # half distance between the bind ankles (balance only)


class Dance:
    def __init__(self, id, name, style, bpm, beats, bpm_range, energy, description="",
                 keys_per_beat=16, beats_per_bar=4, tempo_sync=True):
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
        self.curves = {k: Curve(self.beats, v) for k, v in CONTROL_DEFAULTS.items()}
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
    def pose(self, t, hit=False, tension=1.0, **controls):
        for k, v in controls.items():
            if k not in self.curves:
                raise KeyError("unknown control '%s'" % k)
            self.curves[k].key(t, v, hit=hit, tension=tension)
        return self

    def poses(self, t, preset, hit=False, tension=1.0, **overrides):
        merged = dict(preset)
        merged.update(overrides)
        return self.pose(t, hit=hit, tension=tension, **merged)

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

    def bake(self):
        for f in self.feet.values():
            f.close()
        n = int(round(self.beats * self.kpb)) + 1
        loads = self._loads(n)
        bone_tracks = {}
        hips_track, root_track, foot_tracks = [], [], {"left": [], "right": []}
        hand_tracks = {"left": [], "right": []}
        prev = {}
        stats = {"max_elev": 0}
        for i in range(n):
            t = i / self.kpb
            c = {k: cur.at(t) for k, cur in self.curves.items()}
            li = loads["left"][i % (n - 1)]
            ri = loads["right"][i % (n - 1)]
            if self.roll[0]:
                auto = self.roll[0] * (li - ri)
                c["pelvis_roll"] += auto
                c["spine_side"] -= self.roll[1] * auto
            bones = controls_to_bones(c)
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
            hand_tracks["left"].append((min(1.0, max(0.0, c["hand_l"])),))
            hand_tracks["right"].append((min(1.0, max(0.0, c["hand_r"])),))
            root_track.append((c["root_x"] / REF_HEIGHT_CM, c["root_z"] / REF_HEIGHT_CM, c["root_yaw"] / 360.0))
            for side, f in (("left", fl), ("right", fr)):
                foot_tracks[side].append(
                    (f[0] / REF_HEIGHT_CM, f[1] / REF_HEIGHT_CM, f[2] / REF_HEIGHT_CM, f[3] / 360.0)
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
