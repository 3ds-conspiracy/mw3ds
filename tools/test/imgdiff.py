"""Compares two runs' screenshots (PNG folders from run_test.py --shots), shot by shot: how many pixels of the top
screen differ by more than a small amount, and a picture of where (red) beside each pair.
    python tools/test/imgdiff.py build/shots/skin-cpu build/shots/skin-gpu [--out build/shots/skin-diff] [--max 0.5]
Exit code 1 when a shot differs on more than --max percent of the top screen (default 0.5).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--out", default="")
    ap.add_argument("--max", type=float, default=0.5, help="percent of the top screen allowed to differ")
    ap.add_argument("--tolerance", type=int, default=24, help="per-channel difference that counts")
    args = ap.parse_args()
    a_dir, b_dir = Path(args.a), Path(args.b)
    out = Path(args.out) if args.out else None
    if out:
        out.mkdir(parents=True, exist_ok=True)
    worst = 0.0
    shots = sorted(p.name for p in a_dir.glob("*.png"))
    if not shots:
        sys.exit(f"no shots in {a_dir}")
    for name in shots:
        if not (b_dir / name).exists():
            print(f"{name}: missing in {b_dir}")
            worst = 100.0
            continue
        a = np.asarray(Image.open(a_dir / name).convert("RGB"), dtype=np.int16)[:240]
        b = np.asarray(Image.open(b_dir / name).convert("RGB"), dtype=np.int16)[:240]
        diff = (np.abs(a - b) > args.tolerance).any(axis=2)
        pct = 100.0 * diff.mean()
        worst = max(worst, pct)
        print(f"{name}: {pct:.2f}% of the top screen differs")
        if out:
            mark = a.copy()
            mark[diff] = (255, 0, 0)
            Image.fromarray(np.concatenate([a, b, mark], axis=1).astype(np.uint8)).save(out / name)
    print(f"worst {worst:.2f}% (allowed {args.max}%)")
    sys.exit(0 if worst <= args.max else 1)


if __name__ == "__main__":
    main()
