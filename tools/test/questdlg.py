"""What people say about a quest, with each response's conditions and what it does (journal, items, choices): to work
out the order a player meets a questline's steps in, for the uber quest tests.

  python tools/test/questdlg.py --speaker "ranis athrys"            every response by that speaker
  python tools/test/questdlg.py --topic "join us" [--topic ...]     every response under those topics
  python tools/test/questdlg.py --sets MG_JoinUs                     every response that sets that quest's journal
Conditions print as questrun.conds gives them: (slot, type, op, name, value); JX journal, IX item count, DX dead
count, sX local / global, 50 the choice made, 02 rank, 45 race ...
"""
import argparse
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from mwfiles import load_db  # noqa: E402
from questrun import conds  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speaker", action="append", default=[])
    ap.add_argument("--topic", action="append", default=[])
    ap.add_argument("--sets", action="append", default=[])
    a = ap.parse_args()
    speakers = {s.lower() for s in a.speaker}
    topics = {t.lower() for t in a.topic}
    sets = [q.lower() for q in a.sets]
    if not (speakers or topics or sets):
        print(__doc__)
        return
    for d in load_db()["dialogue"]:
        for i in d["infos"]:
            who = (i.zstr("ONAM") or "").lower()
            res = (i.zstr("BNAM") or "").strip()
            if speakers and who not in speakers:
                continue
            if topics and d["name"].lower() not in topics:
                continue
            if sets and not any(f"journal {q}" in res.lower().replace('"', "") for q in sets):
                continue
            text = (i.zstr("NAME") or "").replace("\n", " ")
            print(f"[{d['name']}] {who or '(anyone)'}: {text[:160]}")
            print(f"    if {conds(i)}")
            if res:
                print("    does " + " | ".join(l.strip() for l in res.splitlines() if l.strip())[:200])


if __name__ == "__main__":
    main()
