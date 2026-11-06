"""Golden tour, OpenMW's half: what a real OpenMW 0.51 says about every cell, for golden tests that involve no dice.

    python tools/test/golden/golden_behave.py tour [--only <text>] [--limit N] [--stall 120] [--no-dialogue]
    python tools/test/golden/golden_behave.py plan
    python tools/test/golden/golden_behave.py case [--only <text>] [--limit N]    (our engine's half of the tour)

The player (god mode, clock stopped) goes to each cell of Morrowind.esm in a fixed order, and after the cell's scripts
have run the mod (mod_behave) reports: every object's enabled state, place, lock, trap and owner; each script's local
variables; each actor's AI package, AI settings, pools, speeds, load, barter gold and disposition; the terrain; rays
down onto the collision; global variables. Every dialogue line is dumped once at the start.

A script can stop the game with a question box, and OpenMW can crash: when no line comes for --stall seconds the run is
ended and restarted from the next cell, the cell named in "stuck". Raw lines go to build/golden/openmw-tour.txt, in
order, tabs between fields. They are derived from the game's data, so they stay in build/ (git-ignored): never commit.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

os.environ["MW3DS_PLUGINS"] = ""       # Morrowind.esm alone, as the OpenMW run loads
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "convert"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import golden_openmw  # noqa: E402
import mwfiles  # noqa: E402

MOD = Path(__file__).resolve().parent / "mod_behave"
WORK = golden_openmw.WORK
TAG = golden_openmw.TAG


def find_cells():
    """Every cell in a fixed order, interiors by name then exteriors by grid: where the player lands in each. An
    interior: where a door into it lands, else on its first object."""
    db = mwfiles.load_db()
    entries, firsts = {}, {}
    for key, cell in db["CELL"].items():
        for r in mwfiles.cell_refs(cell):
            firsts.setdefault(key, r["pos"])
            if r.get("dest_cell") and "dest" in r:
                entries.setdefault(r["dest_cell"].lower(), r["dest"][:3])
    cells = []
    for key in db["CELL"]:
        if isinstance(key, tuple):
            x, y = key
            cells.append({"key": f"ext {x},{y}", "cell": "", "interior": False, "x": x, "y": y,
                          "pos": [(x + 0.5) * 8192, (y + 0.5) * 8192, 0]})
        else:
            pos = entries.get(key.lower()) or firsts.get(key)
            if pos:
                cells.append({"key": key.lower(), "cell": key, "interior": True, "x": 0, "y": 0, "pos": list(pos)})
    return sorted(cells, key=lambda c: (not c["interior"], c["key"] if c["interior"] else (c["x"], c["y"]),
                                        ) if c["interior"] else (True, "", c["x"], c["y"]))


def lua_plan(cells, start, dialogue):
    rows = ["return {", f"\tstart = {start},", f"\tdialogue = {'true' if dialogue else 'false'},", "\tcells = {"]
    for c in cells:
        rows.append("\t\t{ key = %s, cell = %s, interior = %s, x = %d, y = %d, px = %.1f, py = %.1f, z = %.1f }," % (
            json.dumps(c["key"]), json.dumps(c["cell"]), "true" if c["interior"] else "false", c["x"], c["y"],
            c["pos"][0], c["pos"][1], c["pos"][2]))
    return "\n".join(rows + ["\t},", "}", ""])


def run_from(start, cells, dialogue, stall, out):
    """One OpenMW run from cell number start; appends the mod's lines to out. The cell number it reached, and whether
    it got to the end."""
    golden_openmw.MOD = MOD
    user = golden_openmw.write_setup()
    (WORK / "data" / "scripts" / "mw3ds_golden" / "plan.lua").write_text(lua_plan(cells, start, dialogue))
    script = WORK / "startup.txt"
    # (god mode, and the game's run-once script: a run started past the menu does not get it)
    script.write_text("tgm\nStartScript Startup\n")
    logs = [WORK / "openmw.log", user / "openmw.log"]
    for f in logs:
        for _ in range(30):            # (the last run's OpenMW may still be letting go of it)
            try:
                f.unlink(missing_ok=True)
                break
            except PermissionError:
                time.sleep(1)
    exe =golden_openmw.OPENMW / "openmw.exe"
    cmd = [str(exe), "--replace=config", f"--config={WORK}", "--skip-menu", "--start=Seyda Neen", "--no-grab",
           "--no-sound", f"--script-run={script}", "--script-console"]
    proc = subprocess.Popen(cmd, cwd=golden_openmw.OPENMW)
    print(f"openmw: pid {proc.pid}, from cell {start}", flush=True)
    offset, reached, finished, last = 0, start - 1, False, time.time()
    partial = b""
    try:
        while not finished:
            time.sleep(1)
            have = [f for f in logs if f.exists()]
            if have:
                with open(have[0], "rb") as f:
                    f.seek(offset)
                    chunk = f.read()
                offset += len(chunk)
                if chunk:
                    chunk = partial + chunk
                    rows = chunk.split(b"\n")
                    partial = rows.pop()
                    for raw in rows:
                        ln = raw.decode("cp1252", "replace")
                        if TAG not in ln:
                            continue
                        last = time.time()
                        body = ln.split(TAG, 1)[1].strip("\r\n ")
                        out.write(body + "\n")
                        if body.startswith("goto\t"):
                            reached = int(body.split("\t")[1])
                            if reached % 100 == 0:
                                print(f"openmw: cell {reached} of {len(cells)}", flush=True)
                        finished = finished or body.startswith("done")
                    out.flush()
            if proc.poll() is not None and not finished:
                print(f"golden: OpenMW exited at cell {reached}", flush=True)
                break
            if time.time() - last > stall and not finished:
                print(f"golden: no line for {stall} s at cell {reached}", flush=True)
                break
    finally:
        for _ in range(10 if finished else 0):
            if proc.poll() is not None:
                break
            time.sleep(1)
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return reached, finished


def write_case(cells, path):
    """Our engine's half of the tour: the same cells in the same order, the same time in each, and the same stopped
    clock and god mode (native: DUMPTOUR, golden_tour_compare.py)."""
    rows = ["# start: Seyda Neen", "# Written by tools/test/golden/golden_behave.py case: do not edit.",
            "3 0 0 0 0 MSG", "0.1 0 0 0 0 GOD:2+TOURSTART"]
    for c in cells:
        go = "GOTO:" + c["cell"].replace(" ", "_") if c["interior"] else f"COE:{c['x']}:{c['y']}"
        # (a menu a script opened is closed at once, as the OpenMW side has it: scripts wait while one is up)
        rows.append(f"0.1 0 0 0 0 {go}")
        rows.append("0.05 0 0 0 0 SCREEN:none")
        rows.append("0.55 0 0 0")
        rows.append("0.1 0 0 0 0 SCREEN:none+DUMPTOUR:" + c["key"].replace(" ", "%"))
    rows.append("0.1 0 0 0 0 DUMPTOUR")
    path.write_text("\n".join(rows) + "\n", encoding="cp1252")
    print(f"golden: {len(cells)} cells in {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("topic", choices=["tour", "plan", "case"])
    ap.add_argument("--only", default="", help="only cells whose key holds this text")
    ap.add_argument("--limit", type=int, default=0, help="only the first N cells")
    ap.add_argument("--stall", type=int, default=120, help="seconds without a line before moving on")
    ap.add_argument("--resume", type=int, default=0, help="go on from this cell number, adding to --out")
    ap.add_argument("--no-dialogue", action="store_true", help="leave out the dialogue dump")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "golden" / "openmw-tour.txt")
    a = ap.parse_args()
    cells = [c for c in find_cells() if a.only.lower() in c["key"]]
    if a.limit:
        cells = cells[:a.limit]
    if a.topic == "plan":
        for n, c in enumerate(cells, 1):
            print(n, c["key"], [round(v) for v in c["pos"]])
        return
    if not cells:
        sys.exit(f"golden: no cell holds {a.only!r}")
    if a.topic == "case":
        write_case(cells, ROOT / "tools" / "test" / "cases" / "scratch" / "golden-tour.txt")
        return
    if golden_openmw.running():
        sys.exit("golden: an openmw.exe is already running: finish it first")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    start, stuck = a.resume or 1, []
    with open(a.out, "a" if a.resume else "w", encoding="utf-8", newline="\n") as out:
        while start <= len(cells):
            reached, finished = run_from(start, cells, not a.no_dialogue and start == 1, a.stall, out)
            if finished:
                break
            stuck.append(cells[reached - 1]["key"] if reached >= 1 else "(before the first cell)")
            out.write(f"stuck\t{reached}\t{stuck[-1]}\n")
            out.flush()
            if reached < start:        # (never got to a cell: it will not next time either)
                sys.exit(f"golden: OpenMW never reached cell {start}: see {WORK / 'openmw.log'}")
            start = reached + 1
    print(f"golden: {len(cells)} cells, {len(stuck)} stuck {stuck[:20]}; lines in {a.out}")


if __name__ == "__main__":
    main()
