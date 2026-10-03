"""Every converted interior, walked by physim from each door that leads into it: flags doors with no
floor under their destination and rooms the player can get out of.

  python tools/test/physim/sweep.py [walks per door] [seconds per walk] [name filter]
Report: build/physim/sweep.txt (worst first)."""
import json
import re
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from run import BUILD, DATA, ROOT, build, cell_file, raw_cell


def load(path):
    data = path.read_bytes()
    if data[:4] == b"MWZ1":
        data = zlib.decompress(data[8:])
    return json.loads(data)


def main():
    walks = sys.argv[1] if len(sys.argv) > 1 else "40"
    seconds = sys.argv[2] if len(sys.argv) > 2 else "30"
    only = sys.argv[3].lower() if len(sys.argv) > 3 else ""
    data = DATA
    cells = load(data / "game.json")["cells"]
    files = {c["name"].lower(): c["file"] for c in cells if c["interior"]}
    # Door destinations into each interior, from every cell's doors
    starts = {}
    for c in cells:
        path = cell_file(c["file"], ".json")
        if not path.exists():
            continue
        for r in load(path).get("refs", []):
            d = r.get("dest")
            # Doors only: markers (PrisonMarker: where jail sends you) aren't walked through
            if d and d.get("cell") and d["cell"].lower() in files and not d.get("grid") \
                    and "marker" not in r.get("id", "").lower():
                pos = tuple(round(v) for v in d["pos"])
                starts.setdefault(d["cell"].lower(), set()).add(pos)
    exe = build()
    rows = []
    for name, file in sorted(files.items()):
        if only and only not in name:
            continue
        raw = raw_cell(file)
        for pos in sorted(starts.get(name, [])):
            out = subprocess.run([str(exe), str(raw), *map(str, pos), walks, seconds], capture_output=True, text=True).stdout
            spawn = re.search(r"spawn: (.*)", out)
            summary = re.search(r"(\d+) of (\d+) walks escaped", out)
            escaped = int(summary.group(1)) if summary else -1
            nofloor = spawn and "no floor" in spawn.group(1)
            rows.append((nofloor, escaped, name, pos, spawn.group(1) if spawn else "?"))
            print(f"{escaped:3d} escaped  {name}  {pos}  {spawn.group(1) if spawn else '?'}", flush=True)
    rows.sort(key=lambda r: (not r[0], -r[1]))
    report = BUILD / f"sweep-{DATA.name}.txt"
    with open(report, "w") as f:
        f.write(f"{len(rows)} door destinations in {len({r[2] for r in rows})} interiors, {walks} walks x {seconds} s each\n")
        f.write(f"no floor under the destination: {sum(1 for r in rows if r[0])}; "
                f"with escapes: {sum(1 for r in rows if r[1] > 0)}\n\n")
        for nofloor, escaped, name, pos, spawn in rows:
            f.write(f"{'NO FLOOR ' if nofloor else ''}{escaped:3d} escaped  {name}  {pos}  {spawn}\n")
    print(f"report: {report}")


if __name__ == "__main__":
    main()
