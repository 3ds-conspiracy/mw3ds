"""Door graph of converted data: doors whose destination isn't in the data, and interiors the player can't
reach from the start (through doors; the outdoors counts as one connected place).

  python tools/test/doorgraph.py [data folder]        (default out/data; out/world for the whole island)
Report: build/doorgraph.txt"""
import collections
import json
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(path):
    data = path.read_bytes()
    if data[:4] == b"MWZ1":
        data = zlib.decompress(data[8:])
    return json.loads(data)


def main():
    data = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "out" / "data"
    game = load(data / "game.json")
    cells = game["cells"]
    names = {c["name"].lower(): c for c in cells if c["interior"]}
    outside = "<outdoors>"
    edges = collections.defaultdict(set)
    missing, unconverted, doors = [], collections.Counter(), 0
    for c in cells:
        here = c["name"].lower() if c["interior"] else outside
        import zlib as _z
        path = data / "cells" / ("%02x" % (_z.crc32(c["file"].encode("latin-1")) & 63)) / (c["file"] + ".json")
        if not path.exists():
            path = data / "cells" / (c["file"] + ".json")
        if not path.exists():
            continue
        for r in load(path).get("refs", []):
            d = r.get("dest")
            if not d or "marker" in r.get("id", "").lower():
                continue
            doors += 1
            if d.get("unconverted"):
                unconverted[str(d.get("cell"))] += 1
                continue
            there = outside if d.get("grid") else str(d.get("cell", "")).lower()
            if there != outside and there not in names:
                missing.append((c["name"], r["id"], d.get("cell")))
                continue
            edges[here].add(there)
    start = game.get("cell", "").lower()
    start = start if start in names else outside
    seen, todo = {start}, [start]
    while todo:
        for n in edges[todo.pop()]:
            if n not in seen:
                seen.add(n)
                todo.append(n)
    unreachable = sorted(n for n in names if n not in seen)
    out = ROOT / "build" / "doorgraph.txt"
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"{data}: {len(names)} interiors, {doors} doors; from {start}: {len(seen) - 1} interiors reachable\n")
        f.write(f"doors to places not converted: {sum(unconverted.values())} ({len(unconverted)} places)\n")
        f.write(f"doors to a cell missing from the data: {len(missing)}\n")
        for m in missing:
            f.write(f"  MISSING {m[0]}: {m[1]} -> {m[2]}\n")
        f.write(f"unreachable interiors: {len(unreachable)}\n")
        for n in unreachable:
            f.write(f"  {names[n]['name']}\n")
        f.write("not converted (doors):\n")
        for n, k in unconverted.most_common():
            f.write(f"  {k:3d}  {n}\n")
    print(open(out, encoding="utf-8").read()[:1500])


if __name__ == "__main__":
    main()
