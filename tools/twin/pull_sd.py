#!/usr/bin/env python3
"""Mirror the board's SD card for the browser twin (-> web/board/sdseed/, then make_seed.sh).

Every kit's face (kit.json, cover.png, thumb.png) so the twin's kit browser
lists exactly what the board's does; whole kits, samples included, for the few
named with --full (or the first N); every other file under /sdcard/crosspad
except kits and big media, so apps that read the card see the same state.
Rides tools/fs_transfer.py on the board's CDC port.
"""
import argparse, io, os, sys, zipfile
sys.path.insert(0, os.path.join(os.environ.get('CROSSPAD_FIRMWARE_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../platform-idf')), 'tools'))
import fs_transfer as ft

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--port', default='/dev/ttyACM2')
ap.add_argument('--full', type=int, default=6, help='how many kits to copy whole')
ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../web/board', 'sdseed'))
ap.add_argument('--max-other', type=int, default=2 << 20, help='skip non-kit files bigger than this')
a = ap.parse_args()
ser = ft.open_port(a.port)
ft.show_progress = lambda *x: -1
_print = print
quiet = lambda *x, **k: None

def ls(remote):
    ser.write(f"FS_LIST {remote}\n".encode())
    out = []
    for e in ft._read_until_end(ser, None):
        _, kind, size, name = e.split(' ', 3)
        out.append((kind == 'D', int(size), name))
    return out

def pull(remote):
    local = os.path.join(a.out, remote[len('/sdcard/'):])
    os.makedirs(os.path.dirname(local), exist_ok=True)
    if os.path.exists(local): return os.path.getsize(local)
    import builtins
    builtins.print = quiet
    try: ft.receive(ser, f"FS_GET 0 0 {remote}", local, remote)
    finally: builtins.print = _print
    return os.path.getsize(local)

kits = sorted(n for d, _, n in ls('/sdcard/crosspad/kits') if d)
print(f"{len(kits)} kit folders")
total = 0
for i, k in enumerate(kits):
    base = f'/sdcard/crosspad/kits/{k}'
    files = ls(base)
    whole = i < a.full
    for d, size, n in files:
        if d:
            if whole:
                for d2, s2, n2 in ls(f'{base}/{n}'):
                    if not d2: total += pull(f'{base}/{n}/{n2}')
            continue
        if whole or n.lower() in ('kit.json', 'cover.png', 'thumb.png'):
            total += pull(f'{base}/{n}')
    print(f"  {'full' if whole else 'face'}  {k}  ({total >> 10} kB so far)")

def walk(remote):
    global total
    for d, size, n in ls(remote):
        path = f'{remote}/{n}'
        if d:
            if path != '/sdcard/crosspad/kits': walk(path)
        elif size <= a.max_other:
            total += pull(path)
        else:
            print(f"  skip {path} ({size >> 10} kB)")
walk('/sdcard/crosspad')
print(f"done: {total >> 10} kB in {a.out}")
