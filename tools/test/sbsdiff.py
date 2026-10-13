"""Pixel differences between the 3DS and OpenMW halves of side-by-side shots (tools/test/sidebyside.py), for choosing
the threshold that flags a shot for a closer look.

    python tools/test/sbsdiff.py <sidebyside out folder> [--tolerance 24] [--csv diff.csv]

Shots of one animation ("tp-self-00" .. "tp-self-04") form a set. For each set every 3DS point is compared with every
OpenMW point:
  same point (the diagonal)    what a matching pair scores
  other points (off the diagonal)  what a pair a known step apart scores: a threshold must sit between the two
Two measures, each the percent of pixels whose largest channel differs by more than --tolerance (0..255):
  frame   the whole 400 x 240 picture
  motion  only where the set moves: pixels that change between the set's points on either side (the actor's area)
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


def differs(a, b, tol):
    return np.abs(a - b).max(axis=2) > tol


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--tolerance", type=int, default=24)
    ap.add_argument("--csv", default="")
    a = ap.parse_args()
    root = Path(a.folder)
    sets = defaultdict(list)
    for p in sorted((root / "3ds").glob("*.png")):
        m = re.match(r"(.+)-(\d\d)$", p.stem)
        if m and (root / "pc" / p.name).exists():
            sets[m.group(1)].append(p.name)
    rows = []
    for name, files in sets.items():
        ds = [load(root / "3ds" / f) for f in files]
        pc = [load(root / "pc" / f) for f in files]
        n = len(files)
        # where the set moves, on either side (one point against the next)
        motion = np.zeros(ds[0].shape[:2], bool)
        for side in (ds, pc):
            for i in range(n - 1):
                motion |= differs(side[i], side[i + 1], a.tolerance)
        area = max(1, int(motion.sum()))
        frame = np.zeros((n, n))
        moving = np.zeros((n, n))
        for i in range(n):
            for j in range(n):
                d = differs(ds[i], pc[j], a.tolerance)
                frame[i, j] = 100.0 * d.mean()
                moving[i, j] = 100.0 * (d & motion).sum() / area
        print(f"== {name}  (motion region {100.0 * area / motion.size:.1f}% of the frame)")
        for label, m in (("frame", frame), ("motion", moving)):
            print(f"  {label:6s} 3DS point x OpenMW point (% pixels differing)")
            for i in range(n):
                cells = "  ".join(f"{m[i, j]:5.1f}{'*' if i == j else ' '}" for j in range(n))
                print(f"    {i}: {cells}")
            diag = np.diag(m)
            off = m[~np.eye(n, dtype=bool)].reshape(n, n - 1)
            print(f"    same point {diag.mean():5.1f} (max {diag.max():5.1f})   other points {off.mean():5.1f} "
                  f"(min {off.min():5.1f})   closest other per row {' '.join(f'{v:4.1f}' for v in off.min(axis=1))}")
        for i, f in enumerate(files):
            rows.append((name, f, frame[i, i], moving[i, i], np.delete(frame[i], i).min(), np.delete(moving[i], i).min()))
    if a.csv:
        with open(a.csv, "w") as out:
            out.write("set,shot,frame_same,motion_same,frame_closest_other,motion_closest_other\n")
            for r in rows:
                out.write(f"{r[0]},{r[1]},{r[2]:.2f},{r[3]:.2f},{r[4]:.2f},{r[5]:.2f}\n")
        print(f"sbsdiff: {a.csv}")


if __name__ == "__main__":
    main()
