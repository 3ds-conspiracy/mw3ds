"""How a player walks into an interior: the doors to take from the open air, as case steps. For the side-quest cases.

  python tools/test/doorpath.py [--out] "Vivec, Simine Fralinie: Bookseller" ["Another cell" ...]   (--out: the way back out)

Prints the exit spot outside the first building (FLYTO:@x,y,z:beside), then a DOORTO per door. Names exact, case free.
Shortest by number of doors; several equal ways: all printed.
"""
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
from mwfiles import DATA_FILES, cell_refs, read_records  # noqa: E402


def main():
    args = sys.argv[1:]
    leaving = "--out" in args
    want = [a.lower() for a in args if a != "--out"]
    if not want:
        print(__doc__)
        return
    interiors = {}
    doors = collections.defaultdict(list)       # interior cell -> [(dest_cell_lower_or_'', dest pos)]
    ext_in = collections.defaultdict(list)      # interior -> [(door pos in exterior cell, exterior cell id)]
    cells = list(read_records(DATA_FILES / "Morrowind.esm", {"CELL"}))
    for c in cells:
        d = c.get("DATA")
        if struct.unpack("<I", d[:4])[0] & 1:
            interiors[c.id.lower()] = c.id
    for c in cells:
        d = c.get("DATA")
        interior = struct.unpack("<I", d[:4])[0] & 1
        for r in cell_refs(c):
            if "dest" not in r:
                continue
            dc = (r.get("dest_cell") or "").lower()
            if interior:
                doors[c.id.lower()].append((dc if dc in interiors else "", r["dest"][:3], r["pos"]))
            elif dc in interiors:
                ext_in[dc].append((tuple(r["pos"]), c.id))
    for w in want:
        # BFS outwards from the target over interior-to-interior doors until one has a door from the open air
        best = None
        seen = {w: [w]}
        queue = collections.deque([w])
        while queue:
            cur = queue.popleft()
            if ext_in.get(cur):
                best = seen[cur]
                break
            for other, cell in [(k, k) for k, v in doors.items() if any(x[0] == cur for x in v)]:
                if other not in seen:
                    seen[other] = [other] + seen[cur]
                    queue.append(other)
        if not best:
            print(f"# {w}: no way in from outside found")
            continue
        first = best[0]
        spot = next((x[1] for x in doors[first] if x[0] == ""), None)
        print(f"# {interiors[w]}")
        if spot and not leaving:
            print(f"120 0 0 0 0 FLYTO:@{spot[0]:.0f},{spot[1]:.0f},{spot[2]:.0f}:beside")
            print("5 0 0 0 0 UNTIL:effect:10:eq:0")
        name = lambda k: interiors[k].replace(' ', '_').replace(':', '|')
        if leaving:
            # back out from the target: each door to the cell before it, then outside
            for step in reversed(best[:-1]):
                print(f"120 0 0 0 0 DOORTO:{name(step)}")
                print(f"10 0 0 0 0 UNTIL:cell:{name(step)}")
            print("120 0 0 0 0 DOORTO:outside")
            print("5 0 0 0 0 UNTIL:effect:10:eq:0")
            continue
        for step in best:
            print(f"120 0 0 0 0 DOORTO:{name(step)}")
            print(f"10 0 0 0 0 UNTIL:cell:{name(step)}")


if __name__ == "__main__":
    main()
