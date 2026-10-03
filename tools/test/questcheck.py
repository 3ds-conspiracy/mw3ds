"""Quest chains without playing them: for every journal stage of the chosen quests, what sets it
(a dialogue response's result script, or a script) and whether that can happen in the converted game:
the speaker is placed somewhere (or placed by a script), the script is on a placed object or started by
name, and the journal conditions on the response can be met by stages that are themselves reachable.

  python tools/test/questcheck.py [--quest main | "<journal-id regex>"]
Report: build/questcheck-<name>.txt (unreachable stages first)."""
import argparse
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from coverage import MAIN_QUEST
from mwfiles import cell_refs, load_db

ROOT = Path(__file__).resolve().parents[2]
JOURNAL_SET = re.compile(r'(?im)^\s*(?:"?[\w\' ]+"?\s*->\s*)?journal[\s,]+"?([\w\']+)"?[\s,]+(-?\d+)')
PLACED_BY_SCRIPT = re.compile(r'(?i)(?:placeatpc|placeatme|placeitemcell|placeitem|positioncell|position)'
                              r'[\s,]+"?([^",\n]+?)"?[\s,]')
MOVED = re.compile(r'(?im)^\s*"?([^"\n]+?)"?\s*->\s*(?:positioncell|position|enable)\b')
START_SCRIPT = re.compile(r'(?i)startscript[\s,]+"?([\w\']+)"?')


def conditions(info):
    """Journal conditions of a response: [(quest, op, value)] (SCVR type '4')."""
    out, subs = [], info.subs
    for i, (t, d) in enumerate(subs):
        if t != "SCVR":
            continue
        s = d.decode("latin-1")
        if s[1] != "4":
            continue
        val = 0
        if i + 1 < len(subs) and subs[i + 1][0] in ("INTV", "FLTV"):
            nd = subs[i + 1][1]
            val = struct.unpack("<i", nd)[0] if subs[i + 1][0] == "INTV" else struct.unpack("<f", nd)[0]
        out.append((s[5:].lower(), s[4], val))
    return out


def holds(op, a, b):
    return {"0": a == b, "1": a != b, "2": a > b, "3": a >= b, "4": a < b, "5": a <= b}.get(op, False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quest", default="main")
    args = ap.parse_args()
    wanted = re.compile(MAIN_QUEST if args.quest == "main" else args.quest, re.I)
    db = load_db()

    # Where every object is placed, and what scripts put or move there
    placed = defaultdict(set)
    for cell in db["CELL"].values():
        name = cell.zstr("NAME") or "exterior"
        for r in cell_refs(cell):
            placed[r["id"].lower()].add(name)
    # Carried: what placed NPCs, creatures and containers hold (NPCO) is placed with them
    for oid, rec in db["objects"].items():
        where = placed.get(oid.lower())
        if not where:
            continue
        for t, d in rec.subs:
            if t == "NPCO":
                placed[d[4:36].split(bytes(1), 1)[0].decode("latin-1").lower()] |= where
    texts = list(db["SCPT"].values())
    for dial in db["dialogue"]:
        texts += [i.zstr("BNAM") or "" for i in dial["infos"]]
    by_script = set()
    started = set()
    for t in texts:
        by_script |= {m.strip().lower() for m in PLACED_BY_SCRIPT.findall(t)}
        started |= {m.lower() for m in START_SCRIPT.findall(t)}
    attached = defaultdict(set)          # script -> objects carrying it
    for oid, rec in db["objects"].items():
        s = rec.zstr("SCRI") if hasattr(rec, "zstr") else None
        if s:
            attached[s.lower()].add(oid.lower())

    # Stages of the chosen quests
    stages = {}
    for dial in db["dialogue"]:
        if dial["type"] == 4:           # all quests: conditions can name other quests
            idx = sorted({struct.unpack_from("<i", i.get("DATA"), 4)[0] for i in dial["infos"]
                          if i.get("QSTN") is None})
            stages[dial["name"].lower()] = idx

    # What sets each stage
    setters = defaultdict(list)          # (quest, stage) -> [(kind, where, needs, ok, why)]
    for dial in db["dialogue"]:
        if dial["type"] == 4:
            continue
        for info in dial["infos"]:
            text = info.zstr("BNAM") or ""
            for q, v in JOURNAL_SET.findall(text):
                q = q.lower()
                if q not in stages:
                    continue
                who = info.zstr("ONAM")
                if who:
                    w = who.lower()
                    ok = bool(placed.get(w)) or w in by_script
                    why = "" if ok else f"{who} is not placed anywhere"
                    where = f'{dial["name"]} ({who})'
                else:
                    ok, why = True, ""
                    where = f'{dial["name"]} ({info.zstr("FNAM") or info.zstr("CNAM") or info.zstr("RNAM") or "anyone"})'
                setters[(q, int(v))].append(("dialogue", where, conditions(info), ok, why))
    for name, text in db["SCPT"].items():
        for q, v in JOURNAL_SET.findall(text):
            q = q.lower()
            if q not in stages:
                continue
            objs = attached.get(name, set())
            ok = any(placed.get(o) or o in by_script for o in objs) or name in started
            why = "" if ok else "script neither on a placed object nor started by name"
            setters[(q, int(v))].append(("script", name, [], ok, why))

    # Reachability: from every quest at 0, a stage is reachable when a usable setter's journal
    # conditions hold for some reachable stages (other conditions are taken as satisfiable)
    reached = defaultdict(lambda: {0})
    changed = True
    while changed:
        changed = False
        for (q, v), ss in setters.items():
            if v in reached[q]:
                continue
            for kind, where, needs, ok, why in ss:
                if ok and all(any(holds(op, s, val) for s in reached[nq]) for nq, op, val in needs):
                    reached[q].add(v)
                    changed = True
                    break

    rows = []
    for q, idx in sorted(stages.items()):
        if not wanted.search(q):
            continue
        for v in idx:
            ss = setters.get((q, v), [])
            if v == 0 and not ss:
                continue
            if v in reached[q]:
                status = "ok"
            elif not ss:
                status = "NO SETTER"       # also in vanilla: a stage nothing sets (unused)
            elif not any(s[3] for s in ss):
                status = "NOT PLACED"
            else:
                status = "UNREACHABLE"
            detail = "; ".join(f"{k} {w}" + (f" [{why}]" if why else "")
                               + (" needs " + ", ".join(f"{nq}{'= != > >= < <='.split()[int(op)]}{val}"
                                                        for nq, op, val in needs) if needs else "")
                               for k, w, needs, ok, why in ss[:3])
            rows.append((status, f"{q} {v}", detail))
    bad = [r for r in rows if r[0] != "ok"]
    name = "main" if args.quest == "main" else re.sub(r"\W+", "_", args.quest).strip("_")
    report = ROOT / "build" / f"questcheck-{name}.txt"
    quests = len({r[1].split()[0] for r in rows})
    with open(report, "w") as f:
        f.write(f"{quests} quests, {len(rows)} stages, {len(bad)} not reachable "
                f"(NO SETTER: nothing sets it, in vanilla too)\n\n")
        for status, stage, detail in sorted(rows, key=lambda r: r[0] == "ok"):
            f.write(f"{status:12s} {stage:28s} {detail}\n")
    print(f"{quests} quests, {len(rows)} stages, {len(bad)} not reachable; report: {report}")
    for status, stage, detail in bad[:40]:
        print(f"{status:12s} {stage:28s} {detail[:150]}")


if __name__ == "__main__":
    main()
