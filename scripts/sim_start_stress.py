#!/usr/bin/env python3
"""Start the simulator N times in an isolated instance and classify each start.

    scripts/sim_start_stress.py N [--audio isolated|off|on] [--headless] [--gdb]

Each start gets its own scratch profile (CROSSPAD_PROFILE_DIR), control port
(CROSSPAD_REMOTE_PORT, default 19861) and PipeWire node names
(CROSSPAD_INSTANCE), and never takes the desktop's default sink, so it can run
beside a simulator somebody is using. It refuses port 19840.

Per start:
  OK     control port up and {"cmd":"ping"} answered within --timeout seconds
  HANG   port not up in time. Re-run under gdb as a child (SIGINT to the
         inferior after the same timeout) to get all thread stacks.
  CRASH  the process exited first. The LVGL assert backtrace / signal and the
         last log lines are printed; --gdb runs every start under gdb so a
         SIGSEGV comes with a stack even without a core dump.

Logs and gdb output go to --out (default build/sim_stress). Exit 0 when every
start was OK, 1 otherwise, 2 for a bench problem (port busy, no binary).
"""
import argparse
import json
import os
import re
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USER_PORT = 19840
GDB_BASE = ["gdb", "-batch", "-ex",
            "handle SIGUSR1 SIGUSR2 SIGALRM SIG34 SIG35 SIG36 SIGPIPE nostop noprint pass"]


def port_open(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3):
            return True
    except OSError:
        return False


def ping(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.settimeout(3)
            s.sendall(b'{"cmd":"ping"}\n')
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
            return bool(buf.strip())
    except OSError:
        return False


def children(pid):
    out = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def stop(proc, grace=8):
    """Stop a process this script started (and only it)."""
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(grace)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def unlimited_core():
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
    except (ValueError, OSError):
        pass


def seed(args, profile):
    if not args.seed_profile:
        return
    for name in ("preferences.json", "device_preferences.json"):
        src = args.seed_profile / name
        if src.exists():
            shutil.copy(src, profile / name)
    dev = profile / "device_preferences.json"
    if dev.exists():
        doc = json.loads(dev.read_text())
        sd = profile / "sdcard"
        sd.mkdir(exist_ok=True)
        doc["sdcard_path"] = str(sd)
        doc["pw_prev_default_sink"] = ""
        dev.write_text(json.dumps(doc))


def make_env(args, profile):
    env = dict(os.environ)
    env["CROSSPAD_REMOTE_PORT"] = str(args.port)
    env["CROSSPAD_INSTANCE"] = args.instance
    env["CROSSPAD_AUDIO"] = args.audio
    env["CROSSPAD_PROFILE_DIR"] = str(profile)
    env["CROSSPAD_NO_MIDI"] = "1"
    if args.headless:
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_RENDER_DRIVER"] = "software"  # the dummy driver has no accelerated renderer
    return env


def launch(args, env, log, gdb):
    cmd = [str(args.binary)]
    if gdb:
        cmd = GDB_BASE + ["-ex", "run", "-ex", "thread apply all bt 30", "-ex", "kill",
                          "--args"] + cmd
    # stdout to a file is fully buffered: line-buffer it, or a hang's log ends
    # long before the hang.
    cmd = ["stdbuf", "-oL", "-eL"] + cmd
    f = open(log, "wb")
    proc = subprocess.Popen(cmd, cwd=args.binary.parent.parent, env=env, stdout=f,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            preexec_fn=unlimited_core, start_new_session=True)
    return proc, f


def wait_ready(args, proc):
    """-> 'OK' | 'HANG' | 'CRASH'."""
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return "CRASH"
        if port_open(args.port) and ping(args.port):
            return "OK"
        time.sleep(0.25)
    return "HANG"


def tail(path, n=40):
    try:
        return "".join(Path(path).read_text(errors="replace").splitlines(True)[-n:])
    except OSError:
        return ""


def crash_report(log, rc):
    text = Path(log).read_text(errors="replace")
    out = []
    if rc is not None and rc < 0:
        out.append("killed by signal %s" % signal.Signals(-rc).name)
    m = re.search(r"\*\*\* LVGL assert failed.*?(?:\n\n|\Z)", text, re.S)
    if m:
        out.append(m.group(0).strip())
    m = re.search(r"Program received signal.*", text, re.S)
    if m:
        out.append(m.group(0)[:6000])
    if not out:
        out.append("exit code %s; last log lines:\n%s" % (rc, tail(log, 25)))
    return "\n".join(out)


def gdb_hang(args, idx, out):
    """Re-run under gdb; after the timeout interrupt the inferior for stacks."""
    profile = Path(tempfile.mkdtemp(prefix="cp_stress_prof_"))
    seed(args, profile)
    log = out / ("start%03d_gdb.log" % idx)
    try:
        proc, f = launch(args, make_env(args, profile), log, gdb=True)
        verdict = wait_ready(args, proc)
        if verdict == "OK":
            stop(proc)
            return False, "did not reproduce under gdb (port came up)"
        if verdict == "HANG":
            kids = children(proc.pid)
            for k in kids:
                os.kill(k, signal.SIGINT)
            try:
                proc.wait(60)
            except subprocess.TimeoutExpired:
                stop(proc)
        else:
            proc.wait()
        f.close()
        return True, "gdb log: %s" % log
    finally:
        shutil.rmtree(profile, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("n", type=int, help="number of starts")
    ap.add_argument("--binary", type=Path, default=ROOT / "bin" / "CrossPad")
    ap.add_argument("--port", type=int, default=19861)
    ap.add_argument("--instance", default="stress")
    ap.add_argument("--audio", choices=["isolated", "off", "on"], default="isolated",
                    help="CROSSPAD_AUDIO for the instance (default isolated: real audio, own node names, no takeover)")
    ap.add_argument("--timeout", type=float, default=25.0, help="seconds to wait for the control port")
    ap.add_argument("--headless", action="store_true", help="SDL_VIDEODRIVER=dummy + SDL_RENDER_DRIVER=software")
    ap.add_argument("--gdb", action="store_true", help="run every start under gdb (stack on SIGSEGV)")
    ap.add_argument("--no-gdb-on-hang", action="store_true")
    ap.add_argument("--seed-profile", type=Path, metavar="DIR",
                    help="copy preferences.json and device_preferences.json from DIR (e.g. ~/.crosspad) into each "
                         "scratch profile, with sdcard_path pointed at an empty scratch dir and the "
                         "default-sink marker cleared, so a start loads a realistic profile")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "sim_stress")
    args = ap.parse_args()

    if args.port == USER_PORT:
        print("refusing port %d: that is the user's simulator" % USER_PORT)
        return 2
    if args.audio == "on":
        print("note: --audio on lets the instance take over the default sink; use only on a bench")
    if not args.binary.exists():
        print("no binary at %s" % args.binary)
        return 2
    if port_open(args.port):
        print("port %d is already in use" % args.port)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    counts = {"OK": 0, "HANG": 0, "CRASH": 0}
    details = []
    for i in range(1, args.n + 1):
        profile = Path(tempfile.mkdtemp(prefix="cp_stress_prof_"))
        seed(args, profile)
        log = args.out / ("start%03d.log" % i)
        t0 = time.time()
        proc, f = launch(args, make_env(args, profile), log, gdb=args.gdb)
        verdict = wait_ready(args, proc)
        took = time.time() - t0
        extra = ""
        if verdict == "OK":
            if args.gdb:
                for k in children(proc.pid):
                    os.kill(k, signal.SIGTERM)
            else:
                stop(proc)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                stop(proc)
        elif verdict == "CRASH":
            f.flush()
            extra = crash_report(log, proc.returncode)
        else:  # HANG
            if not args.gdb:
                stop(proc)
        if verdict == "HANG" and args.gdb:
            for k in children(proc.pid):
                os.kill(k, signal.SIGINT)
            try:
                proc.wait(60)
            except subprocess.TimeoutExpired:
                stop(proc)
            extra = "stacks in %s" % log
        elif verdict == "HANG" and not args.no_gdb_on_hang:
            reproduced, note = gdb_hang(args, i, args.out)
            extra = note
        f.close()
        shutil.rmtree(profile, ignore_errors=True)
        counts[verdict] += 1
        details.append({"start": i, "result": verdict, "seconds": round(took, 1), "log": str(log)})
        print("[%3d/%d] %-5s %5.1fs  %s" % (i, args.n, verdict, took, log))
        if extra:
            print("        " + extra.replace("\n", "\n        "))
        # the port must be free again before the next start
        for _ in range(40):
            if not port_open(args.port):
                break
            time.sleep(0.25)
        else:
            print("port %d still busy after the instance stopped" % args.port)
            return 2

    print("\nsummary: %s" % json.dumps(counts))
    (args.out / "summary.json").write_text(json.dumps({"counts": counts, "starts": details}, indent=1))
    return 0 if counts["HANG"] == 0 and counts["CRASH"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
