#!/usr/bin/env python3
"""Writes test_loop.wav: two bars of drums over a quiet noise bed, for trying plugin GUIs with
real signal (meters, spectrum displays). 120 BPM, 44.1 kHz, 16-bit stereo, peaking near
-12 dBFS so it is never loud. Pure standard library; fixed random seed, so the file is the same
on every run.

The file carries a 'smpl' chunk with one forward loop over the whole file. Godot's WAV importer
reads it (loop mode "Detect From WAV", the default), so the sound loops with no import settings.
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100
BPM = 120
BARS = 2
BEAT = 60.0 / BPM
N = int(RATE * BEAT * 4 * BARS)

rng = random.Random(1234)
left = [0.0] * N
right = [0.0] * N


def add(start_s, length_s, fn, gain_l=1.0, gain_r=1.0):
    s0 = int(start_s * RATE)
    for i in range(int(length_s * RATE)):
        if s0 + i >= N:
            break
        v = fn(i / RATE)
        left[s0 + i] += v * gain_l
        right[s0 + i] += v * gain_r


def kick(t):
    # A sine sweeping 110 -> 45 Hz, decaying fast: the phase is the integral of the frequency.
    f0, f1, k = 110.0, 45.0, 30.0
    phase = 2 * math.pi * (f1 * t + (f0 - f1) * (1 - math.exp(-k * t)) / k)
    return 0.9 * math.sin(phase) * math.exp(-6 * t)


def snare(t):
    return (0.5 * rng.uniform(-1, 1) + 0.3 * math.sin(2 * math.pi * 190 * t)) * math.exp(-18 * t)


def hat(t):
    return 0.25 * rng.uniform(-1, 1) * math.exp(-60 * t)


for bar in range(BARS):
    for beat in range(4):
        t = (bar * 4 + beat) * BEAT
        if beat in (0, 2):
            add(t, 0.5, kick)
        if beat in (1, 3):
            add(t, 0.3, snare, 0.9, 1.0)
        for eighth in (0, 0.5):
            add(t + eighth * BEAT, 0.08, hat, 0.7, 1.0)

# The noise bed: slightly darkened white noise (one-pole low-pass), so a spectrum shows a full
# band even between hits.
lp_l = lp_r = 0.0
for i in range(N):
    lp_l += 0.15 * (rng.uniform(-1, 1) - lp_l)
    lp_r += 0.15 * (rng.uniform(-1, 1) - lp_r)
    left[i] += 0.12 * lp_l
    right[i] += 0.12 * lp_r

peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
scale = 10 ** (-12 / 20) / peak  # normalise to -12 dBFS

frames = bytearray()
for l, r in zip(left, right):
    frames += struct.pack("<hh", int(l * scale * 32767), int(r * scale * 32767))

out = Path(__file__).with_name("test_loop.wav")
with wave.open(str(out), "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(frames))

# Append the 'smpl' chunk (the wave module cannot write one) and fix the RIFF size. Layout:
# 9 header fields, then one 6-field loop: id, type 0 (forward), start, end (inclusive),
# fraction, play count (0 = forever).
smpl = struct.pack("<9I", 0, 0, int(1e9 / RATE), 60, 0, 0, 0, 1, 0) + struct.pack("<6I", 0, 0, 0, N - 1, 0, 0)
data = bytearray(out.read_bytes())
data += b"smpl" + struct.pack("<I", len(smpl)) + smpl
data[4:8] = struct.pack("<I", len(data) - 8)
out.write_bytes(bytes(data))
print(f"{out.name}: {N / RATE:.1f} s, peak -12 dBFS, looping")
