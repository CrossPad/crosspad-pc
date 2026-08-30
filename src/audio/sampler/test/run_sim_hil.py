#!/usr/bin/env python3
"""HIL-style scenarios against the running PC simulator.

Control goes over the simulator's own TCP port on ONE persistent connection —
the server handles a single client inline in its accept loop, so anything else
that connects (the MCP daemon) locks everyone else out. Hit timing is set here
rather than by tool-call latency.

What is measured is the audio module's own output level (`audio_level`), read
back at ~100 Hz. That is the same shape as reading SMPL_PEAK off a board over
CDC, and it is past everything that matters here: pad logic → event bus →
sample engine → mixer channel → module bus. It says whether the wiring carries
sound; the engine bench under src/audio/sampler/test says whether the samples
themselves come out intact.

(The PipeWire virtual source the module taps OUT1 into would give a real
waveform, but `crosspad_out` exposes no capture ports on this build — pw-link
shows a single `input_FR` and nothing to record from. That is a pre-existing
gap in the virtual-audio path, not something the sampler introduced.)
"""
import json, os, re, socket, sys, time

OUT = os.path.dirname(os.path.abspath(__file__))
fails = []


def check(ok, what, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", what,
                           (" — " + str(detail)) if detail else ""))
    if not ok:
        fails.append(what)


class Sim:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 19840), timeout=20)
        self.f = self.s.makefile("rw")

    def cmd(self, **kw):
        self.f.write(json.dumps(kw) + "\n")
        self.f.flush()
        return json.loads(self.f.readline())

    def hit(self, pad, vel=127):
        self.cmd(cmd="pad_press", pad=pad, velocity=vel)

    def rel(self, pad):
        self.cmd(cmd="pad_release", pad=pad)

    def level(self):
        r = self.cmd(cmd="audio_level", stream=0)
        return max(r.get("left", 0.0), r.get("right", 0.0))

    def stats(self):
        r = self.cmd(cmd="stats")
        return r.get("stats", r)

    def press(self):
        """Press and release with a gap.

        LVGL's encoder indev polls at ~30 ms; a press and release pushed in the
        same tick are never observed as a press, so the click never happens and
        the UI looks frozen. (The `click` command used to have the same flaw;
        it now holds the button for `hold_ms`, 120 by default.)
        """
        self.cmd(cmd="encoder_press")
        time.sleep(0.12)
        self.cmd(cmd="encoder_release")
        time.sleep(0.05)

    def shot(self, path, region="lcd"):
        return self.cmd(cmd="screenshot", file=path, region=region)


def bus_floor(sim, seconds=1.5):
    """What the OUT1 bus carries with the sampler idle.

    Not always zero: the mixer sums the sampler with the audio inputs, and with
    the virtual-sink takeover on, everything the desktop plays arrives on IN1
    and lands on the same bus. That is the feature working, but it makes the
    two scenarios that measure quiet things — the silent tail, and the bottom
    of the velocity ramp — unmeasurable rather than failing. They say so
    instead of blaming the engine.
    """
    end = time.monotonic() + seconds
    peak = 0.0
    while time.monotonic() < end:
        peak = max(peak, sim.level())
        time.sleep(0.01)
    return peak


def skip(what, why):
    print("  [SKIP] %s — %s" % (what, why))


def at(t0, offset):
    """Busy-wait to a precise offset from t0 — sleep() alone is too coarse."""
    while True:
        d = t0 + offset - time.monotonic()
        if d <= 0:
            return
        time.sleep(d / 2 if d > 0.002 else 0)


def envelope(sim, t0, until, hits=None):
    """Poll the output level while firing a schedule. Returns [(t, level)]."""
    env = []
    pending = list(hits or [])
    while time.monotonic() - t0 < until:
        while pending and time.monotonic() - t0 >= pending[0][0]:
            t, pad, vel = pending.pop(0)
            sim.hit(pad, vel)
        env.append((time.monotonic() - t0, sim.level()))
    return env


def onsets(env, thresh=0.02, refractory=0.15):
    """Rising edges through `thresh`, one per refractory window."""
    out, last = [], -1e9
    for i in range(1, len(env)):
        t, v = env[i]
        if v >= thresh and env[i - 1][1] < thresh and t - last > refractory:
            out.append(t)
            last = t
    return out


def window_peak(env, t, width=0.30):
    return max([v for (tt, v) in env if t <= tt < t + width] or [0.0])


# ── Scenarios ────────────────────────────────────────────────────────────

LOG = os.environ.get("CP_SIM_LOG", os.path.join(OUT, "sim.log"))


def log_tail(since=0):
    try:
        with open(LOG, "r", errors="replace") as f:
            f.seek(since)
            return f.read(), f.tell()
    except OSError:
        return "", since


def log_pos():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def wait_for_log(marker, since, timeout):
    """The kit-ready line is the only honest completion signal.

    Watching active_pad_logic instead is a trap: closing the kit gate pops the
    pad-logic stack back to the sampler (it is the base logic), so the launcher
    reports "Sampler" whether a kit loaded or the user backed out.
    """
    t0 = time.monotonic()
    pos = since
    while time.monotonic() - t0 < timeout:
        txt, pos = log_tail(pos)
        for line in txt.splitlines():
            if marker in line:
                return line
        time.sleep(0.1)
    return None


def sc_open_sampler(sim, kit_id=0):
    """Load a kit, then open the sampler app.

    The kit goes in over the control port, not by clicking through a browser of
    seventy-four entries — same reason the device tests use KIT_LOAD. Opening
    the app afterwards is what hands the pads to the sampler: the launcher runs
    the mixer's pad logic, so a pad hit there toggles a mixer channel and never
    reaches a sample. That is worth stating because it looks exactly like a
    dead audio path.
    """
    print("[open] kit_load %d, then the sampler app" % kit_id)
    kl = sim.cmd(cmd="kit_list")
    check(kl.get("ok") and kl.get("count", 0) > 0, "the kit manager found kits",
          "%d kits" % kl.get("count", 0))

    pos = log_pos()
    t0 = time.monotonic()
    r = sim.cmd(cmd="kit_load", kit=kit_id)
    check(r.get("ok"), "kit_load accepted", r)

    st = {}
    for _ in range(80):
        time.sleep(0.25)
        st = sim.cmd(cmd="kit_status")
        if not st.get("loading"):
            break
    load_s = time.monotonic() - t0

    line = wait_for_log("kit ready", pos, 10.0)
    check(line is not None, "the kit reached the engine", "%.1f s" % load_s)
    pads = layers = 0
    if line:
        print("      %s" % line.strip())
        m = re.search(r"(\d+) pads, (\d+) layers", line)
        if m:
            pads, layers = int(m.group(1)), int(m.group(2))
        # A kit that parses but whose samples never load sounds identical to a
        # broken audio path — the pads light up either way.
        check(pads > 0 and layers > 0, "the engine holds the kit's samples",
              "%d pads / %d layers" % (pads, layers))
    print("      kit '%s' (%d of %d), parsed=%s"
          % (st.get("name"), st.get("current"), st.get("count"), st.get("parsed")))

    sim.cmd(cmd="click", x=134, y=263)              # Sampler tile
    time.sleep(2.0)
    apl = sim.stats().get("active_pad_logic")
    check(apl == "Sampler", "the sampler app owns the pads", apl)
    sim.shot(os.path.join(OUT, "gui_sampler_panel.png"))
    return line is not None and apl == "Sampler"


def sc_onset(sim, count=12, gap=0.5, vel=127):
    """Every scheduled hit must raise the output level in its own window.

    Counting threshold crossings does not work here: a real drum sample has
    several transients inside one hit, so a crossing counter finds sixteen
    onsets for twelve hits and then argues with itself about which is which.
    A per-window peak asks the only question that matters — did this hit make
    a sound — and the first sample above the floor inside the window gives the
    latency.
    """
    print("[onset] %d hits every %.0f ms across the pad grid" % (count, gap * 1000))
    sched = [(0.5 + i * gap, i % 16, vel) for i in range(count)]
    t0 = time.monotonic()
    env = envelope(sim, t0, 0.5 + count * gap + 0.8, sched)
    rate = len(env) / (env[-1][0] if env else 1)
    peak = max(v for _, v in env)
    print("  polled %d levels at %.0f Hz, peak %.4f" % (len(env), rate, peak))
    check(peak > 0.02, "the output bus carried sound", "peak %.4f" % peak)

    floor = max(0.01, peak * 0.05)
    silent, lat = [], []
    for i, (t, pad, _) in enumerate(sched):
        win = [(tt, v) for (tt, v) in env if t <= tt < t + gap * 0.8]
        wpeak = max([v for _, v in win] or [0.0])
        first = next((tt for (tt, v) in win if v >= floor), None)
        if wpeak < floor:
            silent.append((i, pad, round(wpeak, 4)))
        if first is not None:
            lat.append((first - t) * 1000)
    lat.sort()
    print("  audible %d/%d  level rise vs schedule: p50=%.1f ms p90=%.1f ms max=%.1f ms"
          % (count - len(silent), count,
             lat[len(lat) // 2] if lat else 0.0,
             lat[int(len(lat) * 0.9)] if lat else 0.0,
             max(lat) if lat else 0.0))
    check(not silent, "every scheduled hit sounded", "silent: %s" % silent)
    # The poll itself is ~10 ms apart, so anything under a couple of poll
    # intervals is the measurement, not the engine.
    check((max(lat) if lat else 0) < 150, "no hit arrived late",
          "worst %.1f ms" % (max(lat) if lat else 0))
    return env


def sc_velocity(sim, pad=0, repeats=3):
    """Louder velocity must be louder, measured over repeats.

    `audio_level` reports the peak of the last rendered block and a poll costs
    ~10 ms, so at 128 frames per block roughly three blocks in four are never
    seen — and the one carrying a drum's initial transient is easy to miss.
    A single hit per velocity therefore produces occasional inversions that are
    the sampler of the meter, not the sampler of the audio: the engine's own
    bench, which compares a rendered waveform rather than a polled meter, puts
    the same ramp within 0.01 dB of the expected curve with no inversion at
    all. Taking the best of a few hits removes the artifact without hiding a
    real one — a velocity that genuinely does not get louder stays low across
    every repeat.
    """
    vels = [16, 32, 48, 64, 80, 96, 112, 127]
    print("[velocity] pad %d, %d-step ramp, best of %d" % (pad, len(vels), repeats))
    floor = bus_floor(sim)
    quiet_bench = floor <= 0.01
    if not quiet_bench:
        print("      idle bus %.3f — another source is on OUT1, so the quiet end "
              "of the ramp is masked; only monotonicity is judged" % floor)
    sched, order = [], []
    t = 0.5
    for v in vels:
        for _ in range(repeats):
            sched.append((t, pad, v))
            order.append((t, v))
            t += 0.45
    t0 = time.monotonic()
    env = envelope(sim, t0, t + 0.6, sched)

    peaks = {}
    for (tt, v) in order:
        peaks[v] = max(peaks.get(v, 0.0), window_peak(env, tt, 0.40))
    for v in vels:
        print("      vel %3d -> peak %.4f" % (v, peaks[v]))

    seq = [peaks[v] for v in vels]
    inversions = [(vels[i - 1], vels[i]) for i in range(1, len(seq))
                  if seq[i] < seq[i - 1] * 0.90]
    rng = (seq[-1] / seq[0]) if seq[0] > 0 else 0
    check(all(p > 0.0 for p in seq), "every velocity produced sound")
    check(not inversions, "louder velocity is never quieter", inversions)
    if quiet_bench:
        check(rng > 4.0, "velocity spans a real dynamic range",
              "%.1fx (%.1f dB)" % (rng, 20 * (rng and __import__("math").log10(rng))))
    else:
        skip("velocity spans a real dynamic range",
             "measured %.1fx against an idle bus of %.3f" % (rng, floor))
    return seq


def sc_silence(sim, seconds=4.0):
    print("[silence] %.0f s idle after the last hit" % seconds)
    floor = bus_floor(sim)
    if floor > 0.01:
        skip("the bus goes quiet between hits",
             "another source is on OUT1 (idle bus %.3f) — mute the mixer inputs "
             "to measure this" % floor)
        return floor
    sim.hit(0, 127); time.sleep(0.05); sim.rel(0)
    time.sleep(3.0)                                  # let the tail finish
    t0 = time.monotonic()
    env = envelope(sim, t0, seconds)
    peak = max(v for _, v in env)
    check(peak < 0.002, "the bus goes quiet between hits — no parked DC",
          "peak %.6f over %.0f s" % (peak, seconds))
    return peak


def sc_storm(sim, rate=25.0, seconds=25.0):
    import random
    rng = random.Random(11)
    print("[storm] %.0f hits/s for %.0f s across 16 pads" % (rate, seconds))
    t0 = time.monotonic()
    n, worst_rt, peak = 0, 0.0, 0.0
    quiet = 0
    while time.monotonic() - t0 < seconds:
        at(t0, n / rate)
        p = rng.randrange(16)
        a = time.monotonic()
        sim.hit(p, rng.randint(40, 127))
        worst_rt = max(worst_rt, time.monotonic() - a)
        if n % 2:
            sim.rel(p)
        n += 1
        if n % 5 == 0:
            lv = sim.level()
            peak = max(peak, lv)
            if lv < 0.005:
                quiet += 1
    print("  sent=%d  worst_hit_roundtrip=%.1f ms  peak=%.4f  quiet_samples=%d/%d"
          % (n, worst_rt * 1000, peak, quiet, n // 5))
    check(peak > 0.02, "still making sound at the end of the storm", "%.4f" % peak)
    check(worst_rt < 0.100, "a hit never blocked the caller",
          "worst %.1f ms" % (worst_rt * 1000))
    check(quiet < (n // 5) * 0.25, "the bus was rarely silent during the storm",
          "%d quiet of %d" % (quiet, n // 5))
    st = sim.stats()
    check(st.get("active_pad_logic") == "Sampler", "the sampler still owns the pads")
    return n


def sc_kit_churn(sim, rounds=8, rate=10.0):
    """Swap kits while the pads keep firing.

    A swap that only works from silence is not the thing being tested, so the
    hits keep going through every one and the run fails if none landed inside a
    swap window.
    """
    import threading, random
    print("[churn] %d kit swaps with pads firing at %.0f/s" % (rounds, rate))
    stop = threading.Event()
    swapping = threading.Event()
    counts = {"hits": 0, "in_swap": 0, "audible": 0}
    lock = threading.Lock()

    def stim():
        rng = random.Random(5)
        while not stop.is_set():
            with lock:
                sim.hit(rng.randrange(16), 110)
                counts["hits"] += 1
                if swapping.is_set():
                    counts["in_swap"] += 1
                if sim.level() > 0.01:
                    counts["audible"] += 1
            time.sleep(1.0 / rate)

    th = threading.Thread(target=stim, daemon=True)
    th.start()

    swapped, failed = 0, []
    worst_s = 0.0
    for r in range(rounds):
        kit = (r + 1) % 74
        with lock:
            pos = log_pos()
            swapping.set()
            resp = sim.cmd(cmd="kit_load", kit=kit)
        t0 = time.monotonic()
        line = wait_for_log("kit ready", pos, 25.0)
        dt = time.monotonic() - t0
        worst_s = max(worst_s, dt)
        swapping.clear()
        if line:
            swapped += 1
        else:
            failed.append(kit)
        time.sleep(0.4)

    stop.set()
    th.join(timeout=3)
    print("  swaps=%d/%d worst_swap=%.1f s hits=%d inside_swap=%d audible=%d"
          % (swapped, rounds, worst_s, counts["hits"], counts["in_swap"],
             counts["audible"]))
    check(swapped == rounds, "every kit swap completed", "failed on %s" % failed)
    check(counts["in_swap"] > 0, "pads actually fired inside the swap windows",
          counts["in_swap"])
    check(counts["audible"] > counts["hits"] * 0.3,
          "the pads kept sounding across the swaps",
          "%d of %d samples audible" % (counts["audible"], counts["hits"]))
    st = sim.stats()
    check(st.get("active_pad_logic") == "Sampler",
          "the sampler still owns the pads after churn", st.get("active_pad_logic"))
    return swapped


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    sim = Sim()
    print("ping:", sim.cmd(cmd="ping"))

    if which in ("all", "gui", "onset", "velocity", "silence", "storm", "churn"):
        if not sc_open_sampler(sim):
            print("RESULT: FAIL (could not reach the sampler panel)")
            sys.exit(1)
    if which in ("all", "onset"):
        sc_onset(sim)
    if which in ("all", "velocity"):
        sc_velocity(sim)
    if which in ("all", "silence"):
        sc_silence(sim)
    if which in ("all", "storm"):
        sc_storm(sim)
    if which in ("all", "churn"):
        sc_kit_churn(sim)

    print("\nRESULT: %s%s" % ("PASS" if not fails else "FAIL",
                              "" if not fails else " — " + "; ".join(fails)))
    sys.exit(1 if fails else 0)
