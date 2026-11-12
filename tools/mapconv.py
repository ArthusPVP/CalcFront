#!/usr/bin/env python3
"""Convert OpenFront maps (map.bin + manifest.json) into CalcFront map files.

Output:
  maps/calc/<VAR>.8xv     appvar to send to the calculator (zx0-compressed tiles)
  maps/pc/<id>.cfm        same layout with raw tiles, read by the PC test harness
  maps/preview/<id>.png   preview of each size variant

File layout (little endian):
  "CFM2"  name[16]  comp(1: 0 raw, 1 zx0)  nvar(1)
  nvar x { w(2) h(2) land(2) scale16(2) offset(2) }   offset = start of that variant's tiles
  nnat(1)  nnat x { fx(2) fy(2) name[14] }            fx/fy = position as a fraction of 65536
  tile blobs

Tile byte, row stride = map width:
  bits 5-6  terrain: 0 water, 1 plains, 2 highland, 3 mountain
  land : bits 0-4 owner (0 in the file), bit 7 fallout
  water: bits 0-2 water body id (1-7 = navigable, 0 = lake / map frame)
The outer frame is always water with body id 0, so ships never leave the map.
"""
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "vendor", "openfront-maps")
OUT_CALC = os.path.join(ROOT, "maps", "calc")
OUT_PC = os.path.join(ROOT, "maps", "pc")
OUT_PREV = os.path.join(ROOT, "maps", "preview")
CONVBIN = os.path.expanduser("~/CEdev/bin/convbin")

# id, appvar name, display name, has a "large" variant
MAPS = [
    ("world", "CFWORLD", "World", True),
    ("europe", "CFEUROPE", "Europe", True),
    ("africa", "CFAFRICA", "Africa", False),  # already at the tile limit
    ("asia", "CFASIA", "Asia", True),
    ("northamerica", "CFNAMER", "North America", True),
    ("southamerica", "CFSAMER", "South America", True),
    ("oceania", "CFOCEAN", "Oceania", False),
    ("mena", "CFMENA", "MENA", True),
    ("britannia", "CFBRITAN", "Britannia", False),
    ("japan", "CFJAPAN", "Japan", False),
    ("blacksea", "CFBLKSEA", "Black Sea", False),
    ("italia", "CFITALIA", "Italia", False),
    ("australia", "CFAUSTRL", "Australia", False),
    ("pangaea", "CFPANGEA", "Pangaea", False),
    ("mars", "CFMARS", "Mars", True),
]

NORMAL_MAXDIM = 256
LARGE_TILES = 64000      # w*h must stay below 65535 (16-bit tile index)
MIN_W = 128              # the game's row lookup needs maps at least 128 wide
MAX_H = 512
MAX_NATIONS = 40
NAME_LEN = 14  # incl. NUL


def downsample(land, mag, w, h):
    """Area-average the full-res land mask / magnitude to w x h."""
    H0, W0 = land.shape
    ys = (np.arange(h + 1) * H0 / h).astype(int)
    xs = (np.arange(w + 1) * W0 / w).astype(int)
    landf = land.astype(np.float64)
    magf = (mag * land).astype(np.float64)
    il = np.pad(landf.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    im = np.pad(magf.cumsum(0).cumsum(1), ((1, 0), (1, 0)))

    def box(ii):
        return (ii[ys[1:, None], xs[None, 1:]] - ii[ys[:-1, None], xs[None, 1:]]
                - ii[ys[1:, None], xs[None, :-1]] + ii[ys[:-1, None], xs[None, :-1]])

    area = (ys[1:] - ys[:-1])[:, None] * (xs[1:] - xs[:-1])[None, :]
    lsum = box(il)
    msum = box(im)
    frac = lsum / np.maximum(area, 1)
    avgmag = np.where(lsum > 0, msum / np.maximum(lsum, 1), 0)
    return frac, avgmag


def label_water(water):
    """4-connected components of water; returns label array + sizes."""
    h, w = water.shape
    lab = np.zeros((h, w), np.int32)
    sizes = [0]
    cur = 0
    for sy in range(h):
        for sx in range(w):
            if water[sy, sx] and lab[sy, sx] == 0:
                cur += 1
                stack = [(sy, sx)]
                lab[sy, sx] = cur
                n = 0
                while stack:
                    y, x = stack.pop()
                    n += 1
                    for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                        if 0 <= ny < h and 0 <= nx < w and water[ny, nx] and lab[ny, nx] == 0:
                            lab[ny, nx] = cur
                            stack.append((ny, nx))
                sizes.append(n)
    return lab, sizes


def build_variant(land0, mag0, w, h):
    frac, avgmag = downsample(land0, mag0, w, h)
    land = frac >= 0.45
    land[0, :] = land[-1, :] = False
    land[:, 0] = land[:, -1] = False

    terr = np.zeros((h, w), np.uint8)
    terr[land] = 1
    terr[land & (avgmag >= 10)] = 2
    terr[land & (avgmag >= 20)] = 3

    # water bodies, ignoring the frame (which stays id 0)
    inner = ~land
    inner[0, :] = inner[-1, :] = False
    inner[:, 0] = inner[:, -1] = False
    lab, sizes = label_water(inner)
    order = sorted(range(1, len(sizes)), key=lambda i: -sizes[i])
    comp = np.zeros((h, w), np.uint8)
    for rank, i in enumerate(order[:7]):
        if sizes[i] < 12:
            break
        comp[lab == i] = rank + 1

    tiles = ((terr << 5) | np.where(land, 0, comp)).astype(np.uint8)
    return tiles, land, terr, comp


def zx0(data):
    tmp_raw = os.path.join(OUT_CALC, "_tmp.raw")
    tmp_zx = os.path.join(OUT_CALC, "_tmp.zx0")
    open(tmp_raw, "wb").write(data)
    subprocess.run([CONVBIN, "-j", "bin", "-k", "bin", "-c", "zx0", "-i", tmp_raw,
                    "-o", tmp_zx, "-l", "1"], check=True, stdin=subprocess.DEVNULL)
    out = open(tmp_zx, "rb").read()
    os.remove(tmp_raw)
    os.remove(tmp_zx)
    return out


def preview(path, terr, land, comp, nations, w, h):
    pal = np.array([[40, 70, 140], [150, 190, 110], [190, 180, 120], [230, 230, 225]], np.uint8)
    img = pal[terr]
    img[(~land) & (comp == 0)] = [30, 50, 100]
    for fx, fy, _ in nations:
        x, y = min(w - 1, fx * w >> 16), min(h - 1, fy * h >> 16)
        img[y, x] = [255, 0, 0]
    sc = max(1, 768 // max(w, h))
    Image.fromarray(img).resize((w * sc, h * sc), Image.NEAREST).save(path)


def convert(mid, var, disp, large):
    d = os.path.join(SRC, mid)
    man = json.load(open(os.path.join(d, "manifest.json")))
    W0, H0 = man["map"]["width"], man["map"]["height"]
    raw = np.frombuffer(open(os.path.join(d, "map.bin"), "rb").read(), np.uint8)
    raw = raw[: W0 * H0].reshape(H0, W0)
    land0 = (raw & 0x80) != 0
    mag0 = (raw & 0x1F).astype(np.float32)

    sizes = []
    sc = max(W0, H0) / NORMAL_MAXDIM
    w, h = min(NORMAL_MAXDIM, max(MIN_W, round(W0 / sc))), min(NORMAL_MAXDIM, round(H0 / sc))
    while w * h > LARGE_TILES:  # tile indices are 16-bit and 0xFFFF means "none"
        w, h = w - 1, h - 1
    sizes.append((w, h))
    if large:
        aspect = W0 / H0
        w = int(math.sqrt(LARGE_TILES * aspect))
        h = int(w / aspect)
        while w * h > LARGE_TILES:
            w -= 1
            h = int(w / aspect)
        assert w >= MIN_W and h <= MAX_H
        sizes.append((w, h))

    variants = []
    for w, h in sizes:
        tiles, land, terr, comp = build_variant(land0, mag0, w, h)
        variants.append((w, h, tiles, land, terr, comp))

    # nations: snap to land on the normal variant, store as fractions
    w, h, _, land, _, _ = variants[0]
    nations = []
    used = []
    for n in man["nations"]:
        cx, cy = n["coordinates"]
        x = min(w - 1, int(cx * w / W0))
        y = min(h - 1, int(cy * h / H0))
        best = None
        for r in range(0, 7):
            for yy in range(max(1, y - r), min(h - 1, y + r + 1)):
                for xx in range(max(1, x - r), min(w - 1, x + r + 1)):
                    if land[yy, xx]:
                        dd = (xx - x) ** 2 + (yy - y) ** 2
                        if best is None or dd < best[0]:
                            best = (dd, xx, yy)
            if best:
                break
        if not best:
            continue
        _, x, y = best
        if any((x - ux) ** 2 + (y - uy) ** 2 < 16 for ux, uy in used):
            continue
        used.append((x, y))
        name = n["name"].encode("ascii", "replace")[: NAME_LEN - 1]
        nations.append((int((x + 0.5) * 65536 / w), int((y + 0.5) * 65536 / h), name))
    if len(nations) > MAX_NATIONS:
        step = len(nations) / MAX_NATIONS
        nations = [nations[int(i * step)] for i in range(MAX_NATIONS)]

    def pack(comp_flag, blobs):
        nvar = len(variants)
        hdr_len = 4 + 16 + 2 + nvar * 10 + 1 + len(nations) * (4 + NAME_LEN)
        out = b"CFM2" + disp.encode()[:15].ljust(16, b"\0") + bytes([comp_flag, nvar])
        off = hdr_len
        for (w, h, tiles, land, _, _), blob in zip(variants, blobs):
            nland = int(land.sum())
            scale = man["map"]["num_land_tiles"] / max(nland, 1)
            out += struct.pack("<HHHHH", w, h, nland, min(65535, int(scale * 16)), off)
            off += len(blob)
        out += bytes([len(nations)])
        for fx, fy, name in nations:
            out += struct.pack("<HH", fx, fy) + name.ljust(NAME_LEN, b"\0")
        assert len(out) == hdr_len
        return out + b"".join(blobs)

    raw_blobs = [v[2].tobytes() for v in variants]
    open(os.path.join(OUT_PC, mid + ".cfm"), "wb").write(pack(0, raw_blobs))
    zx_blobs = [zx0(b) for b in raw_blobs]
    calc = pack(1, zx_blobs)
    assert len(calc) < 65000
    tmp = os.path.join(OUT_CALC, "_tmp.all")
    open(tmp, "wb").write(calc)
    subprocess.run([CONVBIN, "-j", "bin", "-k", "8xv", "-r", "-n", var, "-i", tmp,
                    "-o", os.path.join(OUT_CALC, var + ".8xv"), "-l", "1"], check=True,
                   stdin=subprocess.DEVNULL)
    os.remove(tmp)

    for i, (w, h, _, land, terr, comp) in enumerate(variants):
        preview(os.path.join(OUT_PREV, mid + ("_large" if i else "") + ".png"),
                terr, land, comp, nations, w, h)

    desc = "  ".join(f"{w}x{h} land={int(l.sum())}" for w, h, _, l, _, _ in variants)
    print(f"{mid:14s} {desc}  nations={len(nations):2d} appvar={len(calc):5d}B")


if __name__ == "__main__":
    for dpath in (OUT_CALC, OUT_PC, OUT_PREV):
        os.makedirs(dpath, exist_ok=True)
    want = sys.argv[1:]
    for mid, var, disp, large in MAPS:
        if not want or mid in want:
            convert(mid, var, disp, large)
