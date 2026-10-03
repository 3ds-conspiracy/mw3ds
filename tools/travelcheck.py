"""Every travel destination (silt striders, boats, gondolas, guild guides, Propylon-less: what game.json's
actors offer) checked with physim: the cell is converted and a floor is under the spot.

  MW3DS_OUT=out/world python tools/travelcheck.py
Report: build/travelcheck-<data>.txt"""
import json
import re
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "physim"))
from run import DATA, ROOT, build, cell_file, raw_cell


def load(path):
    data = path.read_bytes()
    if data[:4] == b"MWZ1":
        data = zlib.decompress(data[8:])
    return json.loads(data)


def section(game, name):
    """A game.json section, joined from its parts (game_<name>_<k>.json) when it was split."""
    items = list(game.get(name) or [])
    for part in sorted(DATA.glob(f"game_{name}_*.json"), key=lambda p: int(p.stem.rsplit("_", 1)[1])):
        items += load(part)[name] if isinstance(load(part), dict) else load(part)
    return items


def main():
    game = load(DATA / "game.json")
    cells = game["cells"]
    by_name = {c["name"].lower(): c for c in cells if c["interior"]}
    by_grid = {tuple(c["grid"]): c for c in cells if not c["interior"] and "grid" in c}
    exe = build()
    rows, checked = [], 0
    for a in section(game, "actors"):
        for d in a.get("travel") or []:
            checked += 1
            if d.get("grid"):
                # Outdoors the neighbours are loaded too (a platform may be another cell's object)
                gx, gy = d["grid"]
                cell = by_grid.get((gx, gy))
                around = [by_grid.get((gx + i, gy + j)) for j in (-1, 0, 1) for i in (-1, 0, 1)]
            else:
                cell = by_name.get(d["cell"].lower())
                around = [cell]
            where = f'{a["id"]} -> {d["cell"]} {[round(v) for v in d["pos"]]}'
            if not cell or not cell_file(cell["file"], ".cel").exists():
                rows.append(("NOT CONVERTED", where))
                continue
            text, best = "no floor under the destination", None
            for c in around:
                if not c or not cell_file(c["file"], ".cel").exists():
                    continue
                out = subprocess.run([str(exe), str(raw_cell(c["file"])), *(str(v) for v in d["pos"]), "0", "0"],
                                     capture_output=True, text=True).stdout
                spawn = re.search(r"spawn: floor at (-?\d+) \((-?\d+) below", out)
                if spawn and (best is None or int(spawn.group(1)) > best):
                    best = int(spawn.group(1))
                    text = spawn.group(0)[len("spawn: "):] + f" (in {c['file']})"
            m = re.search(r"\((-?\d+) below", text)
            if "no floor" in text or not m or abs(int(m.group(1))) > 200:
                rows.append(("BAD", f"{where}: {text}"))
            else:
                rows.append(("ok", f"{where}: {text}"))
    bad = [r for r in rows if r[0] != "ok"]
    report = ROOT / "build" / f"travelcheck-{DATA.name}.txt"
    with open(report, "w") as f:
        f.write(f"{checked} travel destinations, {len(bad)} with problems\n\n")
        for status, text in sorted(rows, key=lambda r: r[0] == "ok"):
            f.write(f"{status:14s}{text}\n")
    print(f"{checked} travel destinations, {len(bad)} with problems; report: {report}")
    for status, text in bad:
        print(status, text)


if __name__ == "__main__":
    main()
