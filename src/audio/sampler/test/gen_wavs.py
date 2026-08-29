#!/usr/bin/env python3
"""Test material for the PC sample engine bench.

Everything the scenarios play is generated here so the run is reproducible and
so the edge cases (8-bit, 24-bit, empty data chunk, odd-sized chunk before
data, truncated file) exist as real files rather than as mocks.
"""
import math, os, struct, sys

SR = 48000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sd", "crosspad", "kits", "TEST")
os.makedirs(OUT, exist_ok=True)


def wav16(path, frames_lr, sr=SR, channels=2):
    data = bytearray()
    for fr in frames_lr:
        if channels == 1:
            data += struct.pack("<h", fr[0])
        else:
            data += struct.pack("<hh", fr[0], fr[1])
    _write(path, data, sr, channels, 16)


def _write(path, data, sr, channels, bits, data_override=None, extra_chunk=None,
           truncate=0):
    block = channels * bits // 8
    chunks = b""
    if extra_chunk:
        cid, payload = extra_chunk
        chunks += cid + struct.pack("<I", len(payload)) + payload
        if len(payload) & 1:
            chunks += b"\x00"
    fmt = struct.pack("<HHIIHH", 1, channels, sr, sr * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + chunks
    n = len(data) if data_override is None else data_override
    body += b"data" + struct.pack("<I", n) + bytes(data)
    blob = b"RIFF" + struct.pack("<I", len(body)) + body
    if truncate:
        blob = blob[:-truncate]
    with open(path, "wb") as f:
        f.write(blob)


def p(name):
    return os.path.join(OUT, name)


def sine(seconds, freq=997.0, amp=0.5, sr=SR):
    n = int(seconds * sr)
    return [(int(amp * 32767 * math.sin(2 * math.pi * freq * i / sr)),) * 2 for i in range(n)]


def decay(seconds, freq, amp=0.9, tau=0.08, sr=SR):
    n = int(seconds * sr)
    out = []
    for i in range(n):
        t = i / sr
        v = amp * math.exp(-t / tau) * math.sin(2 * math.pi * freq * t)
        out.append((int(v * 32767),) * 2)
    return out


# ── material the scenarios play ──────────────────────────────────────────
wav16(p("sine997_10s.wav"), sine(10.0))                 # stream integrity + analyze sine
wav16(p("sine997_60s.wav"), sine(60.0))                 # long stream under stress
wav16(p("click.wav"), decay(0.02, 3000.0, tau=0.003))   # sharp transient, onset timing
for i, f in enumerate([60, 90, 140, 200, 260, 320, 420, 520,
                       640, 780, 900, 1100, 1400, 1800, 2300, 3000]):
    wav16(p("pad%02d.wav" % i), decay(0.35, float(f)))  # a 16-pad kit
wav16(p("loop_1s.wav"), sine(1.0, freq=250.0))          # loop material
wav16(p("tiny32.wav"), sine(32 / SR))                   # shorter than one render block
wav16(p("mono44k.wav"), [(int(0.5 * 32767 * math.sin(2 * math.pi * 440 * i / 44100)),)
                         for i in range(44100)], sr=44100, channels=1)

# ── edge cases ───────────────────────────────────────────────────────────
_write(p("bad_8bit.wav"), bytes([128] * 1000), SR, 1, 8)
_write(p("bad_24bit.wav"), bytes(3000), SR, 1, 24)
_write(p("empty_data.wav"), b"", SR, 2, 16)
_write(p("lies_about_size.wav"), struct.pack("<hh", 1000, 1000) * 100, SR, 2, 16,
       data_override=1 << 20)                            # data size past EOF
_write(p("odd_chunk.wav"), struct.pack("<hh", 800, 800) * 4800, SR, 2, 16,
       extra_chunk=(b"LIST", b"INFOodd length here!"))    # odd chunk before data
_write(p("truncated.wav"), struct.pack("<hh", 900, 900) * 4800, SR, 2, 16, truncate=4000)
with open(p("notawav.txt"), "w") as f:
    f.write("this is not a RIFF file\n")

print("generated %d files in %s" % (len(os.listdir(OUT)), OUT))
