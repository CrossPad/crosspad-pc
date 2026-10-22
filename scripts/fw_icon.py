#!/usr/bin/env python3
# A copy of platform-idf's tools/fw_icon.py: the same PNG gives the same icon.bin
# on the board and in the simulator. Change both.
"""Kit icons: the profile's 64x64 PNG -> the 48x48 LVGL image the CrossPad draws.

  fw_icon.py convert config/profiles/icons/full.png -o icon.bin
  fw_icon.py check config/profiles/icons/*.png
  fw_icon.py grid --cells 4 --rgb 255,136,0 -o config/profiles/icons/full.png

The device never decodes a PNG for a kit icon (CLAUDE.md "PSRAM, not internal
RAM"): CI turns the PNG into an LVGL v9 binary image in colour format RGB565A8
-- a 48x48 RGB565 plane, then a 48x48 alpha plane -- 6924 bytes, which the
package manifest hashes and CI signs. Pure Python (zlib only) with integer
arithmetic, so the same PNG gives the same bytes on every machine.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

SRC_SIZE = 64
OUT_SIZE = 48
LV_IMAGE_MAGIC = 0x19          # LV_IMAGE_HEADER_MAGIC, LVGL v9
LV_CF_RGB565A8 = 0x14          # LV_COLOR_FORMAT_RGB565A8
HEADER_LEN = 12                # lv_image_header_t
ICON_BYTES = HEADER_LEN + OUT_SIZE * OUT_SIZE * 2 + OUT_SIZE * OUT_SIZE
ICON_BYTES_MAX = 16 * 1024     # kFwIconBytesMax: what the device accepts for icon.bin
PNG_SIG = b"\x89PNG\r\n\x1a\n"
COLOR_RGB = 2
COLOR_RGBA = 6
RGB_BYTES = 3
RGBA_BYTES = 4
CHUNK_OVERHEAD = 12            # length, kind, CRC
IHDR_LEN = 13
# RGB565: 5 bits red, 6 green, 5 blue, packed little-endian
R_BITS, G_BITS, B_BITS = 5, 6, 5
R_SHIFT, G_SHIFT = G_BITS + B_BITS, B_BITS
COLOR_BITS = 8
# Source pixels are measured in thirds: 64 / 48 = 4 / 3, so each output pixel
# covers exactly 4 thirds of a source pixel per axis -- integer weights.
THIRDS = 3
SPAN = SRC_SIZE * THIRDS // OUT_SIZE
AREA = SPAN * SPAN
GRID_MARGIN = 4
GRID_GAP = 4


assert ICON_BYTES <= ICON_BYTES_MAX   # the device refuses a larger icon.bin


class IconError(ValueError):
    pass


def profile_icon(stem: str) -> str:
    """The one icon path a profile may name: its own PNG in the icons/ next to it."""
    return f"icons/{stem}.png"


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _chunks(data: bytes):
    """(kind, body) of every chunk, each bounds- and CRC-checked; IEND must be there."""
    pos = len(PNG_SIG)
    while pos + CHUNK_OVERHEAD <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 8 + length
        if end + 4 > len(data):
            raise IconError("PNG chunk runs past the end of the file")
        body = data[pos + 8:end]
        if struct.unpack(">I", data[end:end + 4])[0] != zlib.crc32(kind + body) & 0xFFFFFFFF:
            raise IconError(f"PNG chunk {kind!r} fails its CRC")
        yield kind, body
        if kind == b"IEND":
            return
        pos = end + 4
    raise IconError("PNG has no IEND")


def read_png(data: bytes) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    """8-bit RGB or RGBA, not interlaced -> (w, h, RGBA pixels row by row).

    Profile PNGs come from community pull requests, so nothing is allocated
    from a size the file merely claims: the dimensions are checked right after
    IHDR and the inflate is capped at the raw size those dimensions need.
    """
    if not data.startswith(PNG_SIG):
        raise IconError("not a PNG")
    idat = bytearray()
    header = None
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            if len(body) != IHDR_LEN:
                raise IconError("PNG IHDR has the wrong length")
            header = struct.unpack(">IIBBBBB", body)
            if header[:2] != (SRC_SIZE, SRC_SIZE):
                raise IconError(f"icon must be {SRC_SIZE}x{SRC_SIZE}, not {header[0]}x{header[1]}")
        elif kind == b"IDAT":
            idat += body
    if header is None:
        raise IconError("PNG without IHDR")
    w, h, depth, color, _, _, interlace = header
    if depth != 8 or color not in (COLOR_RGB, COLOR_RGBA) or interlace != 0:
        raise IconError("PNG must be 8-bit RGB or RGBA, not interlaced")
    bpp = RGBA_BYTES if color == COLOR_RGBA else RGB_BYTES
    stride = w * bpp
    expected = h * (stride + 1)
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(bytes(idat), expected + 1)
    except zlib.error as e:
        raise IconError(f"PNG data: {e}") from e
    if len(raw) != expected or inflater.unconsumed_tail or not inflater.eof:
        raise IconError("PNG data is not exactly the size its dimensions need")
    pixels: list[tuple[int, int, int, int]] = []
    prev = bytearray(stride)
    off = 0
    for _ in range(h):
        ftype = raw[off]
        line = bytearray(raw[off + 1:off + 1 + stride])
        off += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
            elif ftype != 0:
                raise IconError(f"PNG filter type {ftype}")
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            pixels.append((px[0], px[1], px[2], px[3] if bpp == 4 else 255))
        prev = line
    return w, h, pixels


def write_png(w: int, h: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """RGBA, filter 0 -- for the grid icons and the tests."""
    raw = b"".join(b"\x00" + bytes(c for p in pixels[y * w:(y + 1) * w] for c in p) for y in range(h))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, COLOR_RGBA, 0, 0, 0)
    return PNG_SIG + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def _weights(o: int) -> list[tuple[int, int]]:
    """(source index, weight in thirds) covered by output pixel o on one axis."""
    start, end = o * SPAN, (o + 1) * SPAN
    out = []
    for i in range(start // THIRDS, (end + THIRDS - 1) // THIRDS):
        wgt = min(end, (i + 1) * THIRDS) - max(start, i * THIRDS)
        if wgt > 0:
            out.append((i, wgt))
    return out


def _round_div(num: int, den: int) -> int:
    return (num + den // 2) // den


def convert(png: bytes) -> bytes:
    w, h, px = read_png(png)
    if (w, h) != (SRC_SIZE, SRC_SIZE):
        raise IconError(f"icon must be {SRC_SIZE}x{SRC_SIZE}, not {w}x{h}")
    weights = [_weights(o) for o in range(OUT_SIZE)]
    colour = bytearray()
    alpha = bytearray()
    for oy in range(OUT_SIZE):
        for ox in range(OUT_SIZE):
            sr = sg = sb = sa = 0
            for sy, wy in weights[oy]:
                for sx, wx in weights[ox]:
                    r, g, b, a = px[sy * SRC_SIZE + sx]
                    wa = wx * wy * a
                    sr += r * wa
                    sg += g * wa
                    sb += b * wa
                    sa += wa
            # Premultiplied: a transparent neighbour lends no colour, only less alpha.
            r, g, b = (_round_div(v, sa) if sa else 0 for v in (sr, sg, sb))
            colour += struct.pack("<H", ((r >> (COLOR_BITS - R_BITS)) << R_SHIFT) | ((g >> (COLOR_BITS - G_BITS)) << G_SHIFT)
                                   | (b >> (COLOR_BITS - B_BITS)))
            alpha.append(_round_div(sa, AREA))
    header = struct.pack("<BBHHHHH", LV_IMAGE_MAGIC, LV_CF_RGB565A8, 0, OUT_SIZE, OUT_SIZE, OUT_SIZE * 2, 0)
    out = header + bytes(colour) + bytes(alpha)
    assert len(out) == ICON_BYTES
    return out


def check_bytes(data: bytes) -> list[str]:
    try:
        convert(data)
    except IconError as e:
        return [str(e)]
    return []


def check(path: Path) -> list[str]:
    path = Path(path)
    if not path.is_file():
        return [f"{path}: missing"]
    return [f"{path}: {e}" for e in check_bytes(path.read_bytes())]


def grid_png(cells: int, rgb: tuple[int, int, int]) -> bytes:
    """A cells x cells pad grid on transparency -- the official kits' icon style."""
    size = (SRC_SIZE - 2 * GRID_MARGIN - (cells - 1) * GRID_GAP) // cells
    px = []
    for y in range(SRC_SIZE):
        for x in range(SRC_SIZE):
            inside = False
            for c in range(cells):
                lo = GRID_MARGIN + c * (size + GRID_GAP)
                if lo <= x < lo + size:
                    for r in range(cells):
                        top = GRID_MARGIN + r * (size + GRID_GAP)
                        inside = inside or top <= y < top + size
            px.append((*rgb, 255) if inside else (0, 0, 0, 0))
    return write_png(SRC_SIZE, SRC_SIZE, px)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("png")
    c.add_argument("-o", "--out", required=True)
    k = sub.add_parser("check")
    k.add_argument("pngs", nargs="+")
    g = sub.add_parser("grid")
    g.add_argument("--cells", type=int, required=True)
    g.add_argument("--rgb", required=True, help="R,G,B")
    g.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    if a.cmd == "convert":
        Path(a.out).write_bytes(convert(Path(a.png).read_bytes()))
        return 0
    if a.cmd == "check":
        errors = [e for p in a.pngs for e in check(Path(p))]
        for e in errors:
            print(e, file=sys.stderr)
        return 1 if errors else 0
    r, gr, b = (int(v) for v in a.rgb.split(","))
    Path(a.out).write_bytes(grid_png(a.cells, (r, gr, b)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
