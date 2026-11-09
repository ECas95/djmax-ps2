#!/usr/bin/env python3
"""Host test of the texture path of ps2_ge.c against gim2png.py, on the game's real GIMs.

Run from WSL (needs gcc):
    python3 /mnt/d/DJMX/tools-src/test_ge_tex.py [--src ps2_ge.c] [--per-format N] [--all]

Compares, texel by texel, the stream ps2_ge.c would send to the GS against what gim2png.py
decodes. Exit 0 means every GIM matched and the cache test passed.
"""
import argparse
import collections
import glob
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gim2png  # noqa: E402

ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(ROOT, "port/ps2/ps2_ge.c"))
    ap.add_argument("--per-format", type=int, default=12)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--fb16", action="store_true")
    a = ap.parse_args()

    os.environ["ASAN_OPTIONS"] = "detect_leaks=0"
    tmp = tempfile.mkdtemp(prefix="gex")
    exe = os.path.join(tmp, "t")
    cmd = ["gcc", "-std=gnu11", "-O1", "-g", "-fno-strict-aliasing", "-fsanitize=address,undefined",
           "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-DGE_SRC=\"%s\"" % a.src,
           "-I" + os.path.join(ROOT, "port/ps2"), os.path.join(HERE, "test_ge_tex.c"), "-o", exe, "-lm"]
    if a.fb16:
        cmd.insert(1, "-DGE_FB16=1")
    subprocess.check_call(cmd)

    if subprocess.call([exe, "cache"]):
        print("FAIL: cache test")
        return 1

    files = sorted(glob.glob(os.path.join(ROOT, "analysis/extract/**/*.gim"), recursive=True))
    # Bigger first within a format: more rows, more chunking. Take a spread, not just the big ones.
    by_fmt = collections.defaultdict(list)
    for path in files:
        data = open(path, "rb").read()
        try:
            g = gim2png.GimFile(data, path)
        except ValueError:
            continue
        by_fmt[g.image.format].append((path, data, g))

    done = collections.Counter()
    bad = collections.Counter()
    skipped = collections.Counter()
    for fmt, items in sorted(by_fmt.items()):
        items.sort(key=lambda t: -(t[2].image.width * t[2].image.height))
        if not a.all and len(items) > a.per_format:
            step = len(items) / a.per_format
            items = [items[int(i * step)] for i in range(a.per_format)]
        for path, data, g in items:
            img = g.image
            w, h = img.width, img.height
            if w & (w - 1) or h & (h - 1) or w > 512 or h > 512:
                skipped[fmt] += 1
                continue
            _, _, rgba, _ = gim2png.decode_gim(data, path)
            exp = os.path.join(tmp, "e.rgba")
            with open(exp, "wb") as f:
                f.write(bytes(rgba))
            pix = img.base + img.data_offset()
            pal = (g.palette.base + g.palette.data_offset()) if g.palette is not None else 0
            r = subprocess.run([exe, "gim", path, exp, str(w), str(h), str(fmt), str(pix), str(pal),
                                "1" if img.swizzled else "0"], capture_output=True, text=True)
            done[fmt] += 1
            if r.returncode:
                bad[fmt] += 1
                print("FAIL", os.path.relpath(path, ROOT), r.stdout.strip(), r.stderr.strip()[-400:])
            elif done[fmt] == 1:
                print("first ok:", os.path.relpath(path, ROOT), r.stdout.strip())

    # The disc has only DXT5, and its art rarely reaches some modes: random blocks hit them all
    # (c0 <= c1, a0 <= a1), and DXT1/DXT3, against the same reference.
    rnd = random.Random(1)
    for fmt in (8, 9, 10):
        bsize = 8 if fmt == 8 else 16
        for w, h in ((64, 64), (256, 32), (2, 1), (4, 4), (1, 8)):
            data = bytearray(rnd.getrandbits(8) for _ in range(((w + 3) // 4) * ((h + 3) // 4) * bsize))
            for o in range(0, len(data), bsize * 2):    # equal endpoints sit on the mode boundary
                data[o + 6:o + 8] = data[o + 4:o + 6]
                if bsize == 16:
                    data[o + 15] = data[o + 14]
            data = bytes(data)
            raw, exp = os.path.join(tmp, "r.bin"), os.path.join(tmp, "e.rgba")
            with open(raw, "wb") as f:
                f.write(data)
            with open(exp, "wb") as f:
                f.write(bytes(gim2png.decode_dxt(data, w, h, fmt)))
            r = subprocess.run([exe, "gim", raw, exp, str(w), str(h), str(fmt), "0", "0", "0"],
                               capture_output=True, text=True)
            done["random DXT %d" % fmt] += 1
            if r.returncode:
                bad["random DXT %d" % fmt] += 1
                print("FAIL random", fmt, w, h, r.stdout.strip(), r.stderr.strip()[-400:])

    for fmt in sorted(k for k in done if isinstance(k, int)):
        print("format %2d %-9s: %3d GIMs, %d failed, %d not power of two (skipped)" % (
            fmt, gim2png.FMT_NAMES.get(fmt, "?"), done[fmt], bad[fmt], skipped[fmt]))
    for k in sorted(k for k in done if not isinstance(k, int)):
        print("%s: %d cases, %d failed" % (k, done[k], bad[k]))
    return 1 if sum(bad.values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())
