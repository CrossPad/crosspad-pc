# Watch the board's twin frames (SysEx F0 7D 1F 09) for N seconds, asking for
# them (TWIN 1) as the page would: `python3 twinmon.py 5` (needs python-rtmidi).
import rtmidi, time, sys, collections
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 3
mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
port = [i for i, p in enumerate(mo.get_ports()) if 'Crosspad MIDI 1' in p][0]
mo.open_port(port)
pi = [i for i, p in enumerate(mi.get_ports()) if 'Crosspad MIDI 1' in p][0]
mi.open_port(pi); mi.ignore_types(sysex=False, timing=True, active_sense=True)
frames = []
def cb(ev, _):
    m, _ = ev
    if m[:4] == [0xF0, 0x7D, 0x1F, 0x09]:
        t = (m[5] << 21) | (m[6] << 14) | (m[7] << 7) | m[8]
        frames.append((time.time(), m[4], t, m[9:-1]))
mi.set_callback(cb)
end = time.time() + dur; last = 0
while time.time() < end:
    if time.time() - last > 2:
        mo.send_message([0xF0, 0x7D, 0x1F, 0x04] + list(b'TWIN 1') + [0xF7]); last = time.time()
    time.sleep(0.01)
c = collections.Counter(k for _, k, _, _ in frames)
print('kinds', dict(c))
for f in frames:
    if f[1] != 5: print(f[1], f[2], f[3])
clk = [f[2] for f in frames if f[1] == 5]
if len(clk) > 2: print('clock span', clk[0], clk[-1], 'n', len(clk), 'mean dt', (clk[-1]-clk[0])/(len(clk)-1))
