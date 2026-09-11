#!/usr/bin/env python3
"""Slice C: generate the bundled UI sounds from scratch.

All three WAVs are short ORIGINAL synthesized tones (no sampled audio,
no copyrighted music): plain sine/sine+harmonic voices with exponential
decays and short raised-cosine edge fades so there are no clicks.

Format: 16-bit mono 22050 Hz (keeps the ISO small). Deterministic: no
randomness, so re-running regenerates byte-identical files.

  boot.wav   (~1.5 s) warm two-tone power-on chime (E5 -> B5 overlap)
  chime.wav  (~1.0 s) soft notification ping (A5 + octave shimmer)
  melody.wav (~3.0 s) original 8-note music-box arpeggio
                         C5 E5 G5 B5 | A5 G5 E5 D5  (own melody)

Usage:  python3 scripts/gen_sounds.py
Writes: userspace/boot.wav userspace/chime.wav userspace/melody.wav
Total:  ~265 KB (budget: < 500 KB).
"""

import math
import os
import struct
import wave

RATE = 22050
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "userspace")


def sine_note(freq, dur, vol=0.5, decay=4.0, harmonic=0.0, harm_decay=8.0):
    """One enveloped note: sine + optional 2nd harmonic, exp decay."""
    n = int(RATE * dur)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * decay)
        s = math.sin(2.0 * math.pi * freq * t) * env
        if harmonic > 0.0:
            s += harmonic * math.sin(2.0 * math.pi * freq * 2.0 * t) * math.exp(-t * harm_decay)
        out[i] = s * vol
    return out


def mix_into(buf, note, offset_s):
    off = int(RATE * offset_s)
    need = off + len(note)
    if need > len(buf):
        buf.extend([0.0] * (need - len(buf)))
    for i, v in enumerate(note):
        buf[off + i] += v


def edge_fade(buf, ms=5.0):
    n = min(len(buf), int(RATE * ms / 1000.0))
    for i in range(n):
        g = 0.5 - 0.5 * math.cos(math.pi * i / n)
        buf[i] *= g
        buf[len(buf) - 1 - i] *= g


def normalize(buf, peak=0.89):
    m = max(abs(v) for v in buf) or 1.0
    g = peak / m
    return [v * g for v in buf]


def write_wav(name, buf):
    buf = normalize(buf)
    frames = [int(max(-1.0, min(1.0, v)) * 32767.0) for v in buf]
    path = os.path.join(OUT_DIR, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % len(frames), *frames))
    size = os.path.getsize(path)
    print("%s: %.2fs %d bytes" % (name, len(frames) / RATE, size))
    return size


def gen_boot():
    # Warm power-on: E5 (659.25) then B5 (987.77) overlapping, soft 3rd.
    buf = []
    mix_into(buf, sine_note(659.25, 1.1, vol=0.50, decay=3.0, harmonic=0.25), 0.00)
    mix_into(buf, sine_note(830.61, 0.9, vol=0.22, decay=3.5), 0.12)
    mix_into(buf, sine_note(987.77, 1.2, vol=0.50, decay=3.0, harmonic=0.25), 0.35)
    edge_fade(buf)
    return buf


def gen_chime():
    # Soft notification ping: A5 + octave shimmer, fast bloom, ~1 s tail.
    buf = []
    mix_into(buf, sine_note(880.0, 1.0, vol=0.55, decay=5.0, harmonic=0.30), 0.00)
    mix_into(buf, sine_note(1760.0, 0.6, vol=0.18, decay=7.0), 0.02)
    edge_fade(buf, ms=4.0)
    return buf


def gen_melody():
    # Original music-box arpeggio (own melody, 8 eighth-notes at 100 bpm):
    #   C5 E5 G5 B5 A5 G5 E5 D5, gentle overlap + sparkle octave.
    seq = [523.25, 659.25, 783.99, 987.77, 880.0, 783.99, 659.25, 587.33]
    step = 60.0 / 100.0 / 2.0  # eighth note (~0.30 s) -> ~3.0 s total
    buf = []
    for k, f in enumerate(seq):
        mix_into(buf, sine_note(f, 0.9, vol=0.45, decay=4.5, harmonic=0.35), k * step)
    edge_fade(buf)
    return buf


def main():
    total = 0
    total += write_wav("boot.wav", gen_boot())
    total += write_wav("chime.wav", gen_chime())
    total += write_wav("melody.wav", gen_melody())
    print("total: %d bytes (budget 512000)" % total)
    if total >= 512000:
        raise SystemExit("size budget exceeded")


if __name__ == "__main__":
    main()
