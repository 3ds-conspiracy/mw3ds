"""Converts a run's SHOT screenshots (shot_step_NN.bmp: 400 x 480, top screen above the bottom one) to PNG.
    python tools/test/shots.py <sd folder> <out folder> [--screen both|top|bottom]
run-test.ps1 -Shots <folder> calls it. Prints each PNG written.
"""
import argparse
from pathlib import Path

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--screen", choices=["both", "top", "bottom"], default="both")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    shots = sorted(Path(a.src).glob("shot_step_*.bmp"))
    if not shots:
        print(f"shots: none in {a.src}")
    for bmp in shots:
        img = Image.open(bmp).convert("RGB")
        if a.screen == "top":
            img = img.crop((0, 0, 400, 240))
        elif a.screen == "bottom":
            img = img.crop((40, 240, 360, 480))
        png = out / (bmp.stem + ".png")
        img.save(png)
        print(f"shot: {png}")


if __name__ == "__main__":
    main()
