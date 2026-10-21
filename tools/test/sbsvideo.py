"""Side-by-side videos: the 3DS engine beside OpenMW, moving. Runs tools/test/sidebyside.py on a shots file, then joins
each shot's numbered frames (<out>/<name>-NN.png: the 3DS left, OpenMW right) into <out>/<name>.mp4.

    python tools/test/sbsvideo.py <shots.txt> [--out build/sidebyside/video] [--fps 16] [--loops 3] [--emu]
                                  [--crop x,y,w,h] [--scale 2] [--skip-shots]

A held animation (group= at=0,0.04,...: one frame per point, the actor held still) shows the pose through the cycle;
a scene (do=...;spawn:<id>:x:y:z;travel:<id>:x:y:z with after=0.1,0.1,...) lets OpenMW's world run, the actor
walking on its own AI, one frame every after= seconds (OpenMW side only: --pc-only). --emu draws the 3DS side in
Azahar (the native build leaves some actors and effects out). --crop cuts the same window out of both halves (pixels of
one 400 x 240 half), --scale enlarges it; --loops plays a held cycle several times. --skip-shots joins the frames
already there.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find_ffmpeg():
    """ffmpeg as video.py finds it: NATIVE_FFMPEG, the PATH, or WinGet's Gyan.FFmpeg."""
    exe = os.environ.get("NATIVE_FFMPEG") or shutil.which("ffmpeg")
    if exe:
        return exe
    for p in (Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/WinGet/Packages").glob("Gyan.FFmpeg*/*/bin/ffmpeg.exe"):
        return str(p)
    sys.exit("ffmpeg not found (winget install Gyan.FFmpeg, or set NATIVE_FFMPEG)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("shots")
    ap.add_argument("--out", default="build/sidebyside/video")
    ap.add_argument("--fps", type=float, default=16.0)
    ap.add_argument("--loops", type=int, default=1)
    ap.add_argument("--emu", action="store_true")
    ap.add_argument("--pc-only", action="store_true")
    ap.add_argument("--crop", default="")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--skip-shots", action="store_true")
    ap.add_argument("--data", default="out/world")
    a = ap.parse_args()
    out = ROOT / a.out
    if not a.skip_shots:
        cmd = [sys.executable, str(ROOT / "tools/test/sidebyside.py"), a.shots, "--out", a.out, "--data", a.data]
        cmd += ["--emu"] if a.emu else []
        cmd += ["--pc-only"] if a.pc_only else []
        if subprocess.call(cmd, cwd=ROOT) != 0:
            sys.exit("sidebyside.py failed")
    # (--pc-only writes OpenMW's frames alone, in pc/)
    folder = out / "pc" if a.pc_only else out
    names = sorted({m.group(1) for p in folder.glob("*-[0-9][0-9].png") if (m := re.match(r"(.+)-\d\d\.png$", p.name))})
    if not names:
        sys.exit(f"no numbered frames in {folder}")
    ffmpeg = find_ffmpeg()
    for name in names:
        filters = []
        if a.crop:
            x, y, w, h = (int(v) for v in a.crop.split(","))
            # (the same window out of each half, side by side again)
            half = 400 if not a.pc_only else None
            if half:
                filters.append(f"split[l][r];[l]crop={w}:{h}:{x}:{y}[a];[r]crop={w}:{h}:{x + half}:{y}[b];[a][b]hstack")
            else:
                filters.append(f"crop={w}:{h}:{x}:{y}")
        if a.scale != 1.0:
            filters.append(f"scale=iw*{a.scale}:ih*{a.scale}:flags=neighbor")
        filters.append("pad=ceil(iw/2)*2:ceil(ih/2)*2")
        dest = out / f"{name}.mp4"
        cmd = [ffmpeg, "-v", "error", "-y", "-stream_loop", str(max(0, a.loops - 1)), "-framerate", str(a.fps),
               "-i", str(folder / f"{name}-%02d.png"), "-vf", ",".join(filters), "-c:v", "libx264", "-pix_fmt", "yuv420p",
               str(dest)]
        if subprocess.call(cmd) != 0:
            sys.exit(f"ffmpeg failed on {name}")
        print(f"sbsvideo: {dest}")


if __name__ == "__main__":
    main()
