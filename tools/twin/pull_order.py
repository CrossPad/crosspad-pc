#!/usr/bin/env python3
"""The order the board's card lists its directories in (-> sdseed-order.json).

FAT hands entries back in the order they were written, and everything that
lists the card without sorting (the kit scan above all) shows them in that
order. The twin's card is IndexedDB, which knows no such order: web/simtwin.js
puts each directory back in the board's before the firmware starts. Only
names, so it is quick; run it whenever the seed is refreshed or the board's
card changed. Rides tools/fs_transfer.py on the board's CDC port.
"""
import argparse, json, os, sys
sys.path.insert(0, os.path.join(os.environ.get('CROSSPAD_FIRMWARE_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../platform-idf')), 'tools'))
import fs_transfer as ft

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--port', default='/dev/ttyACM2')
ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../web/board', 'sdseed-order.json'))
a = ap.parse_args()
ser = ft.open_port(a.port)

def ls(remote):
    ser.write(f"FS_LIST {remote}\n".encode())
    out = []
    for e in ft._read_until_end(ser, None):
        _, kind, size, name = e.split(' ', 3)
        out.append((kind == 'D', name))
    return out

order = {}
def walk(remote):
    entries = ls(remote)
    order[remote[len('/sdcard'):] or '/'] = [n for _, n in entries]
    for d, n in entries:
        if d: walk(f'{remote}/{n}')
walk('/sdcard')
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
json.dump(order, open(a.out, 'w'), ensure_ascii=False, separators=(',', ':'))
print(f"{len(order)} directories, {sum(len(v) for v in order.values())} entries -> {os.path.normpath(a.out)}")
