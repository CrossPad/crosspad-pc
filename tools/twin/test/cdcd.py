# The harness's line to the board's CDC: one command per stdin line, the reply,
# then a line "\x00END".
#   !pull <remote> <local>   fs_transfer pull (the port is closed around it)
import serial, sys, time, subprocess
import os
PORT = os.environ.get('TWIN_PORT', '/dev/ttyACM2')
PY = os.environ.get('TWIN_PY', sys.executable)
FS = os.path.join(os.environ.get('CROSSPAD_FIRMWARE_DIR', os.path.join(os.path.dirname(__file__), '../../../../platform-idf')), 'tools/fs_transfer.py')
def op():
    for _ in range(50):
        try: return serial.Serial(PORT, 115200, timeout=0.05)
        except Exception: time.sleep(0.2)
    raise SystemExit('no port')
s = op()
for line in sys.stdin:
    c = line.rstrip('\n')
    if c.startswith('!pull '):
        _, r, l = c.split(' ', 2)
        s.close()
        out = subprocess.run([PY, FS, '-p', PORT, 'pull', r, l], capture_output=True, text=True)
        s = op()
        print((out.stdout + out.stderr).strip().splitlines()[-1] if (out.stdout + out.stderr).strip() else 'pull?')
        print('\x00END', flush=True); continue
    s.reset_input_buffer(); s.write((c + '\n').encode())
    buf = b''; t0 = time.time(); last = time.time()
    while time.time() - t0 < 15:
        d = s.read(4096)
        if d: buf += d; last = time.time()
        elif buf.endswith(b'\n') and time.time() - last > 0.12: break
    print(buf.decode(errors='replace').strip())
    print('\x00END', flush=True)
