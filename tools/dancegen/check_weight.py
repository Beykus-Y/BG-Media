#!/usr/bin/env python3
"""Weight over the support foot, measured on the authored clips.

    python tools/dancegen/check_weight.py [--dances assets/dances] [--json out.json]

In single support (exactly one foot in contact) the pelvis should sit over the planted
foot. For every key of such a step the horizontal distance between the hips track and
the support ankle (bind ankle + foot track, cm) is taken. The report gives the median and
the 90th percentile per clip. groove, hiphop and sidestep are gated: median <= 4 cm and
p90 <= 7 cm. Exit status 1 if a gated clip fails.

The clips store lengths as fractions of a 160 cm character; foot tracks are 5 values per
key (x, y, z, yaw, toe) and hips are 3 values per key.
"""

import argparse
import json
import math
import os
import sys

REF_HEIGHT_CM = 160.0
FOOT_HALF_CM = 7.0          # half the distance between the bind ankles (bgdance.FOOT_HALF_CM)
GATED = ("groove", "hiphop", "sidestep")
MEDIAN_MAX_CM = 4.0
P90_MAX_CM = 7.0


def contact(clip, foot, beat):
    for seg in clip["contacts"]:
        if seg["foot"] == foot and seg["start"] <= beat <= seg["end"]:
            return True
    return False


def percentile(sorted_vals, q):
    """Nearest-rank percentile of an already sorted list."""
    rank = int(math.ceil(q * len(sorted_vals))) - 1
    return sorted_vals[max(0, min(len(sorted_vals) - 1, rank))]


def measure(clip):
    """-> (offsets_cm sorted, worst (offset_cm, beat), lateral_cm, sagittal_cm sorted)."""
    kpb = clip["keysPerBeat"]
    n = int(round(clip["beats"] * kpb)) + 1
    hips = clip["tracks"]["hips"]
    feet = {"left": clip["tracks"]["footLeft"], "right": clip["tracks"]["footRight"]}
    offsets, lateral, sagittal = [], [], []
    worst = (0.0, 0.0)
    for i in range(n - 1):
        beat = i / float(kpb)
        left, right = contact(clip, "left", beat), contact(clip, "right", beat)
        if left == right:
            continue  # double support, or a foot in the air on both sides
        if left:
            sx, sz = FOOT_HALF_CM + feet["left"][5 * i] * REF_HEIGHT_CM, feet["left"][5 * i + 2] * REF_HEIGHT_CM
        else:
            sx, sz = -FOOT_HALF_CM + feet["right"][5 * i] * REF_HEIGHT_CM, feet["right"][5 * i + 2] * REF_HEIGHT_CM
        hx, hz = hips[3 * i] * REF_HEIGHT_CM, hips[3 * i + 2] * REF_HEIGHT_CM
        off = math.hypot(hx - sx, hz - sz)
        offsets.append(off)
        lateral.append(abs(hx - sx))
        sagittal.append(abs(hz - sz))
        if off > worst[0]:
            worst = (off, beat)
    offsets.sort()
    lateral.sort()
    sagittal.sort()
    return offsets, worst, lateral, sagittal


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    parser = argparse.ArgumentParser()
    parser.add_argument("--dances", default=os.path.join(root, "assets", "dances"))
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    names = ["groove", "hiphop", "sidestep", "energetic", "idle"]
    report = {}
    failed = []
    print("%-10s %6s %8s %8s %8s %8s %8s   %s" % (
        "clip", "keys", "median", "p90", "max", "lat p90", "sag p90", "gate"))
    for name in names:
        with open(os.path.join(args.dances, name + ".json"), encoding="utf-8") as f:
            clip = json.load(f)
        offsets, worst, lateral, sagittal = measure(clip)
        if not offsets:
            print("%-10s %6d %s" % (name, 0, "no single-support keys"))
            report[name] = {"keys": 0}
            continue
        med = percentile(offsets, 0.5)
        p90 = percentile(offsets, 0.9)
        gated = name in GATED
        ok = med <= MEDIAN_MAX_CM and p90 <= P90_MAX_CM
        if gated and not ok:
            failed.append(name)
        gate = ("PASS" if ok else "FAIL") if gated else "info"
        print("%-10s %6d %8.2f %8.2f %8.2f %8.2f %8.2f   %s" % (
            name, len(offsets), med, p90, offsets[-1], percentile(lateral, 0.9),
            percentile(sagittal, 0.9), gate))
        report[name] = {
            "keys": len(offsets), "median_cm": round(med, 3), "p90_cm": round(p90, 3),
            "max_cm": round(offsets[-1], 3), "worst_beat": round(worst[1], 3),
            "gated": gated, "pass": ok,
        }
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
    print("limits (gated clips): median <= %.1f cm, p90 <= %.1f cm" % (MEDIAN_MAX_CM, P90_MAX_CM))
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
