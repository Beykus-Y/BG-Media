#!/usr/bin/env python3
"""Frame-by-frame capture of the REAL BG Media Desktop application.

Every frame is a separate launch of bgmedia_desktop.exe with --capture and
--capture-time, so each image is exactly what the application renders for that
media time (same retargeter, same SDL_GPU renderer, same UI). This is NOT a
screen recording: the result is a series of deterministic stills, assembled
into a contact sheet and an animated GIF at the real frame spacing.

  python scripts/capture_dance_sequence.py --name groove_cycle \
      --args "--dance groove --dance-bpm 96" --times 0:10:20 --bpm 96

--view presentation   clean full-window frames cropped around the dancer (default)
--view ui             the whole application window
"""

import argparse
import os
import shlex
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont


def parse_times(spec):
    if ":" in spec:
        a, b, n = spec.split(":")
        a, b, n = float(a), float(b), int(n)
        return [a + (b - a) * i / n for i in range(n)]
    return [float(x) for x in spec.split(",")]


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--args", default="", help="extra bgmedia_desktop arguments")
    ap.add_argument("--times", required=True, help="start:end:count or a,b,c (media seconds)")
    ap.add_argument("--bpm", type=float, default=0.0, help="label frames with beats at this tempo")
    ap.add_argument("--view", default="presentation", choices=["presentation", "ui"])
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--scale", type=float, default=0.42)
    ap.add_argument("--crop", default="400,40,1040,880", help="x0,y0,x1,y1 of the dancer in the 1440x900 window")
    ap.add_argument("--model", default=os.path.join(root, "models", "Model1.vrm"))
    ap.add_argument("--exe", default=os.path.join(root, "build", "windows", "bin", "bgmedia_desktop.exe"))
    ap.add_argument("--out", default=os.path.join(root, "artifacts", "dance-captures"))
    ap.add_argument("--gif", action="store_true")
    ap.add_argument("--gif-ms", type=int, default=0, help="GIF frame duration (default: real spacing)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    frames_dir = os.path.join(args.out, args.name + "_frames")
    os.makedirs(frames_dir, exist_ok=True)
    times = parse_times(args.times)
    extra = shlex.split(args.args)
    crop = tuple(int(v) for v in args.crop.split(","))
    images = []
    for i, t in enumerate(times):
        bmp = os.path.join(frames_dir, "f%03d.bmp" % i)
        cmd = [args.exe, "--vrm", args.model, "--3d", "--no-audio"] + extra
        if args.view == "presentation":
            cmd.append("--presentation")
        cmd += ["--capture", bmp, "--capture-time", "%.4f" % t]
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=root)
        if result.returncode != 0 or not os.path.exists(bmp):
            print("capture failed at t=%.3f:\n%s" % (t, result.stderr[-500:]))
            sys.exit(1)
        img = Image.open(bmp).convert("RGB")
        if args.view == "presentation":
            img = img.crop(crop)
        images.append(img)
        os.remove(bmp)
        print("frame %2d/%d  t=%.2f s" % (i + 1, len(times), t), flush=True)

    w, h = images[0].size
    sw, sh = int(w * args.scale), int(h * args.scale)
    cols = min(args.cols, len(images))
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGB", (sw * cols, sh * rows), (8, 10, 16))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 13)
    except OSError:
        font = ImageFont.load_default()
    for i, img in enumerate(images):
        tile = img.resize((sw, sh), Image.LANCZOS)
        x, y = (i % cols) * sw, (i // cols) * sh
        sheet.paste(tile, (x, y))
        label = "%.2fs" % times[i]
        if args.bpm > 0:
            label += "  beat %.1f" % (times[i] * args.bpm / 60.0)
        draw.rectangle([x + 4, y + 4, x + 8 + 8 * len(label), y + 21], fill=(0, 0, 0))
        draw.text((x + 6, y + 5), label, fill=(120, 240, 210), font=font)
    sheet_path = os.path.join(args.out, args.name + "_sheet.png")
    sheet.save(sheet_path)
    print("wrote", sheet_path)
    if args.gif:
        spacing = args.gif_ms or int(round((times[1] - times[0]) * 1000)) if len(times) > 1 else 100
        gif = [im.resize((int(w * 0.5), int(h * 0.5)), Image.LANCZOS) for im in images]
        gif_path = os.path.join(args.out, args.name + ".gif")
        gif[0].save(gif_path, save_all=True, append_images=gif[1:], duration=spacing, loop=0, optimize=True)
        print("wrote", gif_path, "(%d ms per frame, real capture spacing)" % spacing)


if __name__ == "__main__":
    main()
