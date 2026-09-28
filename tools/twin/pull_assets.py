#!/usr/bin/env python3
"""Mirror the board's assets partition for the browser twin (-> boardassets/).

The twin draws its icons and images from what the board has in /assets, not
from the source trees: an assets partition older than the firmware (OTA never
rewrites it) shows blank tiles on the board, and the twin has to show the
same. The web-twin build preloads web/board/assets/ when it holds a pull
(the .pulled stamp; reconfigure after the first one). Rides platform-idf's
tools/fs_transfer.py on the board's CDC port.
"""
import argparse, os, shutil, sys
sys.path.insert(0, os.path.join(os.environ.get('CROSSPAD_FIRMWARE_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../platform-idf')), 'tools'))
import fs_transfer as ft

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--port', default='/dev/ttyACM2')
ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../web/board', 'assets'))
a = ap.parse_args()
ser = ft.open_port(a.port)
ft.show_progress = lambda *x: -1

def ls(remote):
    ser.write(f"FS_LIST {remote}\n".encode())
    out = []
    for e in ft._read_until_end(ser, None):
        _, kind, size, name = e.split(' ', 3)
        out.append((kind == 'D', int(size), name))
    return out

tmp = a.out + '.new'
shutil.rmtree(tmp, ignore_errors=True)
n = total = 0
def walk(remote, local):
    global n, total
    os.makedirs(local, exist_ok=True)
    for d, size, name in ls(remote):
        if d:
            walk(f'{remote}/{name}', os.path.join(local, name))
        elif name != 'stm32_fw.bin':          # the STM image: the twin has no STM to flash
            import builtins
            _p, builtins.print = builtins.print, (lambda *x, **k: None)
            try: ft.receive(ser, f"FS_GET 0 0 {remote}/{name}", os.path.join(local, name), f'{remote}/{name}')
            finally: builtins.print = _p
            n += 1; total += size
walk('/assets', tmp)
shutil.rmtree(a.out, ignore_errors=True)
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
os.rename(tmp, a.out)
open(os.path.join(a.out, '.pulled'), 'w').close()   # what the build relinks on
print(f"{n} files, {total >> 10} kB in {os.path.normpath(a.out)}")
