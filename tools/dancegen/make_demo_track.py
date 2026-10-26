#!/usr/bin/env python3
"""Synthesises an ORIGINAL demo track (no samples, no third-party audio) whose
energy changes by section, so BG Motion's Auto mode has something to react to:

    bars  1- 8  calm        soft kick on every beat, quiet off-beat hat
    bars  9-16  active      kick on every beat, snare on 2 and 4, 8th hats
    bars 17-24  intense     four-on-the-floor, sub bass, clap, 16th hats, louder
    bars 25-28  calm outro  like the intro
    then ~3 s of silence

    python tools/dancegen/make_demo_track.py [out.wav] [bpm]
"""

import math
import struct
import sys
import wave

import numpy as np

SR = 44100


def env_exp(n, tau):
    t = np.arange(n) / SR
    return np.exp(-t / tau)


def kick(gain=1.0):
    n = int(SR * 0.28)
    t = np.arange(n) / SR
    freq = 120.0 * np.exp(-t * 18.0) + 45.0
    phase = 2 * math.pi * np.cumsum(freq) / SR
    return gain * np.sin(phase) * env_exp(n, 0.09)


def snare(gain=1.0, rng=np.random.default_rng(3)):
    n = int(SR * 0.2)
    noise = rng.standard_normal(n)
    tone = np.sin(2 * math.pi * 190.0 * np.arange(n) / SR)
    return gain * (0.6 * noise + 0.4 * tone) * env_exp(n, 0.05)


def hat(gain=1.0, rng=np.random.default_rng(5)):
    n = int(SR * 0.05)
    noise = rng.standard_normal(n)
    noise = np.diff(noise, prepend=0.0)  # crude high-pass
    return gain * 0.5 * noise * env_exp(n, 0.012)


def bass(freq, dur, gain=1.0):
    n = int(SR * dur)
    t = np.arange(n) / SR
    return gain * np.sin(2 * math.pi * freq * t) * np.minimum(1.0, t * 80) * np.exp(-t / (dur * 0.9))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "artifacts/fixtures/dance_demo_100.wav"
    bpm = float(sys.argv[2]) if len(sys.argv) > 2 else 100.0
    beat = 60.0 / bpm
    bars = 28
    total = bars * 4 * beat + 3.0
    mix = np.zeros(int(SR * (total + 1.0)))

    def put(sample, time):
        i = int(time * SR)
        mix[i:i + len(sample)] += sample[: len(mix) - i]

    for bar in range(bars):
        section = "calm" if bar < 8 else "active" if bar < 16 else "intense" if bar < 24 else "calm"
        for b in range(4):
            t0 = (bar * 4 + b) * beat
            if section == "calm":
                if True:
                    put(kick(0.75), t0)
                put(hat(0.16), t0 + beat * 0.5)
            elif section == "active":
                put(kick(0.9), t0)
                if b in (1, 3):
                    put(snare(0.6), t0)
                put(hat(0.3), t0)
                put(hat(0.26), t0 + beat * 0.5)
            else:
                put(kick(1.0), t0)
                if b in (1, 3):
                    put(snare(0.7), t0)
                    put(snare(0.35), t0 + 0.012)
                for k in range(4):
                    put(hat(0.28 if k % 2 == 0 else 0.2), t0 + beat * k / 4.0)
                put(bass(55.0 if bar % 2 == 0 else 73.4, beat * 0.9, 0.9), t0)
    mix /= max(1e-6, np.max(np.abs(mix))) / 0.85
    pcm = (np.clip(mix, -1, 1) * 32767).astype("<i2")
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print("wrote %s: %.1f s, %.0f bpm, %d bars" % (out, len(pcm) / SR, bpm, bars))


if __name__ == "__main__":
    main()
