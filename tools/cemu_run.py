#!/usr/bin/env python3
"""Drive a headless CEmu: send CalcFront + maps + the CE C libraries, launch the
game, run a key script and save screenshots as PNG.

  CF_ROM=/path/to/your.rom tools/cemu_run.py <outdir> <script>

Environment:
  CF_ROM         TI-84 Plus CE ROM image (dump it from your own calculator)
  CEMU_HEADLESS  cemu-headless binary built with tools/cemu-headless-launch.patch
                 (default: cemu-headless from PATH)
  CE_CLIBS       folder with the CE C libraries (*.8xv), from
                 https://github.com/CE-Programming/libraries/releases/latest

script: one command per line, as understood by cemu-headless (run, key, keys...),
plus "shot <name>" to save <outdir>/<name>.png. Lines starting with # are ignored.
"""
import glob
import os
import shutil
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.environ.get("CF_ROM")
EMU = os.environ.get("CEMU_HEADLESS") or shutil.which("cemu-headless")
CLIBS = os.environ.get("CE_CLIBS", "")
if not ROM or not EMU or len(sys.argv) != 3:
    sys.exit(__doc__)

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
cmds = ["run 3000"]
for f in sorted(glob.glob(os.path.join(CLIBS, "*.8xv"))):
    cmds.append(f"send-file archive {f}")
for f in sorted(glob.glob(os.path.join(ROOT, "maps", "calc", "*.8xv"))):
    cmds.append(f"send-file archive {f}")
cmds.append(f"send-file archive {os.path.join(ROOT, 'bin', 'CALCFRNT.8xp')}")
cmds += ["run 500", "launch CALCFRNT", "run 2500"]
shots = []
for line in open(sys.argv[2]):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    if line.startswith("shot "):
        name = line.split()[1]
        bmp = os.path.join(out, name + ".bmp")
        cmds.append(f"screenshot {bmp}")
        shots.append(bmp)
    else:
        cmds.append(line)
cmds.append("quit")
r = subprocess.run([EMU, "--rom", ROM], input="\n".join(cmds) + "\n", capture_output=True,
                   text=True, timeout=1800)
errs = [l for l in r.stdout.splitlines() if l.startswith("ERR")]
dbg = [l for l in r.stderr.splitlines() if not l.startswith("[CEmu]")]
if errs:
    print("\n".join(errs))
if dbg:
    print("\n".join(dbg[-40:]))
for bmp in shots:
    if os.path.exists(bmp):
        Image.open(bmp).save(bmp[:-4] + ".png")
        os.remove(bmp)
        print("saved", bmp[:-4] + ".png")
