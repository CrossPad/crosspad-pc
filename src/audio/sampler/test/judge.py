#!/usr/bin/env python3
"""Judge the bench captures with the same analyzers used on hardware."""
import os, sys, json
sys.path.insert(0, os.path.expanduser("~/GIT/crosspad-hil"))
from crosspad_hil.analyze import (analyze_sine, analyze_onset, analyze_velocity,
                                  analyze_silence, analyze_click)

OUT = os.environ["CP_OUT"]
rc = 0

def sched(name):
    out = []
    for line in open(os.path.join(OUT, name)):
        t, v = line.split()
        out.append({"t": float(t), "vel": int(v)})
    return out

def show(tag, v, keys):
    global rc
    print(f"\n== {tag}: {v.verdict.upper()} ==")
    for r in v.reasons:
        print("   reason:", r)
    d = v.__dict__
    for k in keys:
        if k in d and d[k] not in (None, {}, []):
            print(f"   {k} = {d[k]}")
    if v.verdict == "fail":
        rc = 1

show("sine (stream integrity, 997 Hz looped)",
     analyze_sine(os.path.join(OUT, "sine_rt.wav"), freq=997.0),
     ["est_freq_hz", "freq_offset_pct", "rms_dbfs", "peak_dbfs", "glitches",
      "glitch_per_s", "timing_slips", "net_drift_samples", "phase_resid_p2p_rad"])

show("onset (16 scheduled hits)",
     analyze_onset(os.path.join(OUT, "onset.wav"), expected=sched("onset_schedule.txt")),
     ["expected", "matched", "missed", "extra", "latency_ms", "noise_floor_dbfs",
      "offset_ms"])

show("velocity (8-step ramp)",
     analyze_velocity(os.path.join(OUT, "velocity.wav"), expected=sched("velocity_schedule.txt")),
     ["expected", "matched", "missed", "dynamic_range_db", "monotonic_inversions",
      "latency_ms", "curve"])

show("silence (tail after one hit)",
     analyze_silence(os.path.join(OUT, "silence.wav")),
     ["rms_dbfs", "peak_dbfs", "glitches", "glitch_per_s"])

sys.exit(rc)
