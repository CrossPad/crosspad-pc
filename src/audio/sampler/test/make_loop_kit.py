#!/usr/bin/env python3
"""The LOOPTEST kit the `loop` scenario of run_sim_hil.py plays.

Pad 1 (index 0) is a Loop pad (playbackMode 2) whose sample is one bar at
120 BPM -- two seconds -- of a 1 kHz sine whose amplitude ramps linearly from
0.05 to 0.95 across the bar, so the level heard says where in the loop the
voice is: launched on the bar line, or unmuted in phase, it follows the bar's
own phase. Pad 2 is a one-shot, pad 3 a Hold pad with a sample longer than
any hold. Written at the engine's rate (44.1 kHz unless --rate): the engine
transposes a sample at another rate rather than resampling it, which would
stretch the bar.

  make_loop_kit.py <kits dir> [--rate 44100]
"""
import argparse, json, math, os, struct, wave

BPM = 120
BEATS_PER_BAR = 4
TONE_HZ = 1000
RAMP_FROM, RAMP_TO = 0.05, 0.95
FULL_SCALE = 32000


def write_wav(path, rate, frames, fn):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        buf = bytearray()
        for i in range(frames):
            v = int(max(-1.0, min(1.0, fn(i))) * FULL_SCALE)
            buf += struct.pack("<hh", v, v)
        w.writeframes(bytes(buf))


def pad(samples, mode, color):
    return {"color": color, "color_pressed": 0xFFFFFF, "choke_group": 0, "cycle": False,
            "pitch": 0, "volume": 100, "pan": 0, "playbackMode": mode, "samples": samples}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kits")
    ap.add_argument("--rate", type=int, default=44100)
    a = ap.parse_args()
    rate = a.rate
    out = os.path.join(a.kits, "LOOPTEST")
    os.makedirs(out, exist_ok=True)

    bar = int(rate * 60 / BPM * BEATS_PER_BAR)
    write_wav(os.path.join(out, "LOOP.wav"), rate, bar,
              lambda i: (RAMP_FROM + (RAMP_TO - RAMP_FROM) * i / bar)
              * math.sin(2 * math.pi * TONE_HZ * i / rate))
    write_wav(os.path.join(out, "SHOT.wav"), rate, int(0.3 * rate),
              lambda i: 0.8 * math.exp(-i / (0.08 * rate)) * math.sin(2 * math.pi * 440 * i / rate))
    write_wav(os.path.join(out, "HOLD.wav"), rate, 3 * rate,
              lambda i: 0.5 * math.sin(2 * math.pi * 330 * i / rate))

    pads = [pad(["LOOP.wav"], 2, 0x00C0FF), pad(["SHOT.wav"], 0, 0xFF4000), pad(["HOLD.wav"], 1, 0x40FF40)]
    pads += [pad([], 0, 0x203020) for _ in range(16 - len(pads))]
    kit = {"name": "LOOPTEST", "author": "crosspad-pc bench", "description": "", "version": "1.0",
           "created_at": "", "type": 0, "pads": pads}
    with open(os.path.join(out, "kit.json"), "w") as f:
        json.dump(kit, f)
    print("LOOPTEST:", bar, "frames a bar at", rate, "Hz in", out)


if __name__ == "__main__":
    main()
