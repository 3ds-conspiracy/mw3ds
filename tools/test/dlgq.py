"""Who teaches a topic, and what a speaker says: for the side-quest cases (tools/test/cases/side).

  python tools/test/dlgq.py --teach "little secret"      every response whose text mentions that topic (who says it, under which topic)
  python tools/test/dlgq.py --about "Quest_Id"           every response that sets or tests that journal id, with conditions
  python tools/test/dlgq.py --speaker "name" [--topic t] full text of what that speaker says (under that topic)
Add --where to list, after the responses, where each speaker stands (cell, position) and where the items they
add or remove lie. --len N: characters of each response (default 260).
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from mwfiles import DATA_FILES, cell_refs, load_db, read_records  # noqa: E402
from questrun import conds  # noqa: E402

ITEM = re.compile(r"""(?:additem|removeitem)\s+"?([\w' -]+?)"?\s+\d""", re.I)


def where(names):
    print("-- where (first 3 of each; gold left out)")
    names = names - {"gold_001"}
    seen = {}
    for c in read_records(DATA_FILES / "Morrowind.esm", {"CELL"}):
        d = c.get("DATA")
        interior = struct.unpack("<I", d[:4])[0] & 1
        for r in cell_refs(c):
            i = (r["id"] or "").lower()
            if i in names and seen.get(i, 0) < 3:
                seen[i] = seen.get(i, 0) + 1
                x, y, z = r["pos"]
                place = "in " + c.id if interior else "outside (cell %d,%d)" % struct.unpack("<ii", d[4:12])
                print(f"{i}: {place} @{x:.0f},{y:.0f},{z:.0f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teach")
    ap.add_argument("--about")
    ap.add_argument("--speaker")
    ap.add_argument("--topic")
    ap.add_argument("--len", type=int, default=260)
    ap.add_argument("--where", action="store_true")
    a = ap.parse_args()
    if not (a.teach or a.about or a.speaker or a.topic):
        print(__doc__)
        return
    names, seen = set(), set()
    for d in load_db()["dialogue"]:
        for i in d["infos"]:
            who = (i.zstr("ONAM") or "").lower()
            res = (i.zstr("BNAM") or "").strip()
            text = (i.zstr("NAME") or "").replace("\n", " ")
            if a.teach and a.teach.lower() not in text.lower():
                continue
            if a.about and a.about.lower() not in (res + " " + str(conds(i))).lower().replace('"', ""):
                continue
            if a.speaker and who != a.speaker.lower():
                continue
            if a.topic and d["name"].lower() != a.topic.lower():
                continue
            key = (d["name"], text[:60], str(conds(i)), res[:80], "" if a.speaker else who if who else "")
            if key in seen and not a.speaker:
                continue
            seen.add(key)
            if who:
                names.add(who)
            names.update(m.group(1).lower().strip() for m in ITEM.finditer(res))
            print(f"[{d['name']}] {who or '(anyone)'}: {text[:a.len]}")
            print(f"    if {conds(i)}")
            if res:
                print("    does " + " | ".join(l.strip() for l in res.splitlines() if l.strip())[:300])
    if a.where:
        where(names)


if __name__ == "__main__":
    main()
