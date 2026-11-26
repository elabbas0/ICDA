#!/usr/bin/env python3
"""Render every sound ICDA ships.

The synthesis lives in tools/soundgen/soundgen.c (one dependency-free C
file; its header describes the sound design).  This script builds it with
the host C compiler in a temporary directory and runs it, so nothing is
left in the tree but the WAVs:

  resources/audio/*.wav   18 interface sounds, 16-bit stereo 48000 Hz
  userspace/boot.wav      (~1.5 s) the tonic chord unfolding upward
  userspace/chime.wav     (~0.9 s) three soft steps up the scale
  userspace/melody.wav    (~2.9 s) original eight-note lullaby
                          Ab4 C5 Eb5 F5 | Eb5 C5 Bb4 Ab4  (own melody)
                          these three: 16-bit mono 22050 Hz

Everything is synthesized (no sampled audio, no copyrighted music) and
deterministic: re-running regenerates byte-identical files.  soundgen
refuses to write a file larger than the one it replaces.

Usage:  python3 scripts/gen_sounds.py        (CC=... to pick the compiler)
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SOURCE = os.path.join(ROOT, "tools", "soundgen", "soundgen.c")
UI_DIR = os.path.join(ROOT, "resources", "audio")
TRACK_DIR = os.path.join(ROOT, "userspace")


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "soundgen")
        subprocess.run([cc, "-O2", "-o", binary, SOURCE, "-lm"], check=True)
        subprocess.run([binary, UI_DIR, TRACK_DIR], check=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as err:
        sys.exit(err.returncode or 1)
