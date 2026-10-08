"""Where can a player get an item? Every place Morrowind.esm puts it: lying in a cell, in a placed container, carried
by a placed NPC or creature (merchants marked: they sell what they carry), in a leveled list. For the uber quest tests
(tools/test/uber/*.src), which must find quest items in play rather than be given them.

  python tools/test/wherefind.py <item id> [<item id> ...]
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
from mwfiles import DATA_FILES, cell_refs, read_records  # noqa: E402

# AIDT services flags that mean "sells things" (OpenMW's AiData: weapons ... misc, potions, magic items)
SELLS = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x0400 | 0x2000


def zid(d):
    return d.split(b"\0", 1)[0].decode("latin-1").lower()


def main():
    want = {a.lower() for a in sys.argv[1:]}
    if not want:
        print(__doc__)
        return
    holders = defaultdict(list)          # holder id -> [(item, count)]
    kind, sells, levels = {}, {}, defaultdict(list)
    cells = []
    for r in read_records(DATA_FILES / "Morrowind.esm"):
        if r.tag in ("CONT", "NPC_", "CREA"):
            hid = r.id.lower()
            kind[hid] = r.tag
            for tag, d in r.subs:
                if tag == "NPCO":
                    item = zid(d[4:36])
                    if item in want:
                        holders[hid].append((item, struct.unpack("<i", d[:4])[0]))
                if tag == "AIDT" and len(d) >= 12:
                    sells[hid] = struct.unpack_from("<I", d, 8)[0] & SELLS
        elif r.tag == "LEVI":
            cur = None
            for tag, d in r.subs:
                if tag == "INAM":
                    cur = zid(d)
                    if cur in want:
                        levels[cur].append(r.id)
        elif r.tag == "CELL":
            cells.append(r)
    for c in cells:
        data = c.get("DATA")
        interior = struct.unpack("<I", data[:4])[0] & 1
        name = c.id if interior or c.id else ""
        if not interior:
            gx, gy = struct.unpack("<ii", data[4:12])
            name = f"{c.id or 'exterior'} ({gx},{gy})"
        for ref in cell_refs(c):
            rid = (ref["id"] or "").lower()
            p = ref["pos"]
            at = f"{name} @{p[0]:.0f},{p[1]:.0f},{p[2]:.0f}"
            if rid in want:
                print(f"{rid}: lying in {at}")
            for item, n in holders.get(rid, []):
                what = {"CONT": "container", "NPC_": "NPC", "CREA": "creature"}[kind[rid]]
                shop = " (merchant: sells it)" if kind[rid] != "CONT" and sells.get(rid) else ""
                print(f"{item}: {what} {rid} x{n}{shop} in {at}")
    for item, lists in levels.items():
        print(f"{item}: leveled lists {', '.join(lists)}")


if __name__ == "__main__":
    main()
