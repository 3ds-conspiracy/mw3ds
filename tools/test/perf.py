"""Sums up a run's per-second profile lines (main.cpp: "profile per frame", "fps ...", "heap: ...") from a game log:
fps, frame work, update parts, batches, free linear memory, heap. For comparing builds on the same route (perf-balmora).
    python tools/test/perf.py <log.txt> [--save build/perf/perf-balmora-o3ds.txt] [--skip 10]
--skip: seconds left out at the start (the load and the first cell streaming).
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

PROFILE = re.compile(r"profile per frame: update ([\d.]+)ms \(world ([\d.]+) player ([\d.]+) stream ([\d.]+) ai ([\d.]+) "
                     r"scripts ([\d.]+) other ([\d.]+)(?: detect ([\d.]+) fx ([\d.]+) greet ([\d.]+) sound ([\d.]+) "
                     r"tail ([\d.]+))?\) actors drawn ([\d.]+)ms, batches (\d+) drawn (\d+) culled")
# (the draw line: the frame's drawing by pass, the UI pieces drawn, the hot paths' call counts; newer logs only)
DRAW = re.compile(r"profile draw: wait ([\d.]+) sky ([\d.]+) cull ([\d.]+) opaque ([\d.]+) distant ([\d.]+) layers ([\d.]+) "
                  r"actors ([\d.]+) blend ([\d.]+) extras ([\d.]+) hud ([\d.]+) bottom ([\d.]+) map ([\d.]+) ms, "
                  r"ui draws top (\d+) bottom (\d+), calls disp (\d+) rays (\d+) floors (\d+) steps (\d+) objects (\d+)"
                  r"(?:, replay (\d+)/(\d+))?")
FPS = re.compile(r"\] fps ([\d.]+) cpu ([\d.]+)ms gpu ([\d.]+)ms linearFree (\d+)KB heapUsed (\d+)KB gpu commands peak (\d+)%")
WORK = re.compile(r"view distance (\d+)%, frame work ([\d.]+)ms")
HEAP = re.compile(r"heap: (\d+)KB used, (\d+)KB peak, (\d+)KB in the biggest free block")
UPDATE_KEYS = ("update", "world", "player", "stream", "ai", "scripts", "other", "detect", "fx", "greet", "sound", "tail",
               "actors", "drawn", "culled")
DRAW_KEYS = ("wait", "sky", "cull", "opaque", "distant", "layers", "actorsdraw", "blend", "extras", "hud", "bottom", "map",
             "uitop", "uibottom", "disp", "rays", "floors", "steps", "objects", "replay", "replaymiss")


def stats(values):
    if not values:
        return "-"
    return f"mean {statistics.mean(values):.2f}  median {statistics.median(values):.2f}  min {min(values):.2f}  max {max(values):.2f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--save", default="")
    ap.add_argument("--skip", type=int, default=10)
    args = ap.parse_args()
    lines = Path(args.log).read_text(errors="replace").splitlines()
    cols = {k: [] for k in (*UPDATE_KEYS, *DRAW_KEYS, "fps", "cpu", "linear", "heap", "cmd", "view", "work", "peak", "largest")}
    seconds = 0
    for line in lines:
        if m := FPS.search(line):
            seconds += 1
            if seconds <= args.skip:
                continue
            for k, v in zip(("fps", "cpu", "linear", "heap", "cmd"), (m[1], m[2], m[4], m[5], m[6])):
                cols[k].append(float(v))
        elif seconds <= args.skip:
            continue
        elif m := PROFILE.search(line):
            for k, v in zip(UPDATE_KEYS, m.groups()):
                if v is not None:
                    cols[k].append(float(v))
        elif m := DRAW.search(line):
            for k, v in zip(DRAW_KEYS, m.groups()):
                if v is not None:
                    cols[k].append(float(v))
        elif m := WORK.search(line):
            cols["view"].append(float(m[1]))
            cols["work"].append(float(m[2]))
        elif m := HEAP.search(line):
            cols["peak"].append(float(m[2]))
            cols["largest"].append(float(m[3]))
    head = [l for l in lines if "MW3DS start" in l][:1]
    out = [f"log: {args.log}", *(h.split("] ", 1)[-1] for h in head), f"seconds: {seconds} ({args.skip} skipped)"]
    names = {"fps": "fps", "cpu": "CPU ms (citro3d)", "work": "frame work ms", "update": "update ms", "world": "  world",
             "player": "  player", "stream": "  stream", "ai": "  ai", "scripts": "  scripts", "other": "  other",
             "detect": "  detect", "fx": "  fx", "greet": "  greet", "sound": "  sound", "tail": "  tail",
             "actors": "actors drawn ms", "drawn": "batches drawn", "culled": "batches culled", "cmd": "GPU commands peak %",
             "view": "view distance %", "linear": "linear free KB", "heap": "heap used KB", "peak": "heap peak KB",
             "largest": "biggest free block KB",
             "wait": "draw ms: wait for GPU", "sky": "  sky", "cull": "  cull", "opaque": "  opaque", "distant": "  distant",
             "layers": "  layers + doors", "blend": "  blend", "extras": "  extras", "hud": "  top HUD", "bottom": "  bottom screen",
             "map": "  local map", "uitop": "UI draws top (peak)", "uibottom": "UI draws bottom (peak)",
             "disp": "calls/frame: disposition", "rays": "  rays", "floors": "  floor samples", "steps": "  actor steps",
             "objects": "  object lookups", "replay": "right eyes from the record/s", "replaymiss": "  drawn again/s"}
    for k, label in names.items():
        if cols[k]:
            out.append(f"{label:24} {stats(cols[k])}")
    text = "\n".join(out)
    print(text)
    if args.save:
        Path(args.save).parent.mkdir(parents=True, exist_ok=True)
        Path(args.save).write_text(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
