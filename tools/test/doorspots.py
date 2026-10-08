"""Where a player stands outside a building or dungeon: the exit spot of each door that leads out of the interiors
named (the interior door's destination, where the game puts the player who walks out). The uber quest tests fly or
walk to these spots, then DOORTO in: a door's own position can be its mesh's origin, inside the walls or the rock.
Also lists the exterior doors into those cells, with the exterior cell they stand in.

  python tools/test/doorspots.py "Balmora, Guild of Mages" Sulipund ...     (interior names, or how they start)
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
from mwfiles import DATA_FILES, cell_refs, read_records  # noqa: E402


def main():
    want = [a.lower() for a in sys.argv[1:]]
    if not want:
        print(__doc__)
        return
    for c in read_records(DATA_FILES / "Morrowind.esm", {"CELL"}):
        d = c.get("DATA")
        interior = struct.unpack("<I", d[:4])[0] & 1
        for r in cell_refs(c):
            if interior and c.id.lower().startswith(tuple(want)) and "dest" in r and not r.get("dest_cell"):
                x, y, z = r["dest"][:3]
                print(f"{c.id}: exit spot outside {x:.0f},{y:.0f},{z:.0f}  (FLYTO:@{x:.0f},{y:.0f},{z:.0f})")
            dc = (r.get("dest_cell") or "").lower()
            if not interior and dc.startswith(tuple(want)):
                gx, gy = struct.unpack("<ii", d[4:12])
                x, y, z = r["pos"]
                print(f"{r['dest_cell']}: door {r['id']} at {x:.0f},{y:.0f},{z:.0f} in exterior cell ({gx},{gy})")


if __name__ == "__main__":
    main()
