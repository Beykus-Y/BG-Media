#!/usr/bin/env python3
"""Regenerates the BG Motion 3.0 dance library (assets/dances/*.json).

    python tools/dancegen/generate_dances.py [--out assets/dances] [--only groove]

The output is fully deterministic: running this script again yields byte-wise
identical files. Blender is NOT required; this uses only the Python standard
library. See docs/BG_MOTION_3.md ("Adding your own dance").
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bgdance import Dance, mirror  # noqa: E402

import dances  # noqa: E402
import dances_b  # noqa: E402

REGISTRY = dances.REGISTRY + dances_b.REGISTRY


def main():
    parser = argparse.ArgumentParser()
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    parser.add_argument("--out", default=os.path.join(root, "assets", "dances"))
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for name, builder in REGISTRY:
        if args.only and name != args.only:
            continue
        dance = builder()
        path = os.path.join(args.out, name + ".json")
        data = dance.write(path)
        print("wrote %-28s %2d beats @ %3d bpm  %7.1f KiB" % (
            os.path.relpath(path, root), data["beats"], data["bpm"], os.path.getsize(path) / 1024.0))


if __name__ == "__main__":
    main()
