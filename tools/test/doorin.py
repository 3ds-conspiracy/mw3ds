"""Where to stand to go into a building: every door into that cell (position, and the cell it is in: the exterior cell,
or the interior to pass through first) and where its own doors lead (exit spots, for FLYTO:@x,y,z:beside). For the side-quest cases.

  python tools/test/doorin.py "Vivec, Simine Fralinie: Bookseller" ["Another, Cell" ...]    (names exact, case free)
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
from mwfiles import DATA_FILES, cell_refs, read_records  # noqa: E402


def main():
    want = {a.lower() for a in sys.argv[1:]}
    if not want:
        print(__doc__)
        return
    for c in read_records(DATA_FILES / "Morrowind.esm", {"CELL"}):
        d = c.get("DATA")
        interior = struct.unpack("<I", d[:4])[0] & 1
        for r in cell_refs(c):
            dc = (r.get("dest_cell") or "").lower()
            if dc in want and "dest" in r:
                x, y, z = r["pos"]
                where = "exterior cell %d,%d '%s'" % (*struct.unpack("<ii", d[4:12]), c.id) if not interior else "inside '%s'" % c.id
                print(f"{r['dest_cell']}: in by {r['id']} at {x:.0f},{y:.0f},{z:.0f} ({where})")
            if interior and c.id.lower() in want and "dest" in r:
                x, y, z = r["dest"][:3]
                print(f"{c.id}: out through {r['id']} to {r.get('dest_cell') or 'the wilds'} at {x:.0f},{y:.0f},{z:.0f}")


if __name__ == "__main__":
    main()
