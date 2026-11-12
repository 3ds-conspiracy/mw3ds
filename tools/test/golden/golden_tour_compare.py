"""Sets our engine's tour (native run of cases/scratch/golden-tour.txt, DUMPTOUR) beside OpenMW's (golden_behave.py).

    python tools/test/golden/golden_tour_compare.py [--log build/native-golden/sd/3ds/mw3ds/log.txt]
        [--golden build/golden/openmw-tour.txt] [--show 12] [--only <text>] [--json build/golden/tour-diff.json]
        [--tribunal] [--bloodmoon]      (the add-ons' tour: openmw-tour-tribunal-bloodmoon.txt and its own diff file)

Compared, cell by cell, only cells both sides reached:
  obj     objects by (id, place): present on both sides, enabled, scale, count, lock, trap, owner. Actors are compared
          by id only, they move. A leveled spot's pick is dice, so spawned actors only count as present if placed.
  sv      each script's local variables: whole numbers exactly, a fraction (mostly timers) is listed apart
  actor   by id, as multisets: AI package, foes, AI settings, pools, speeds, barter gold, disposition, dead
  global  global variables at the end
A difference is grouped by what differs; each group shows --show examples. Nothing in the output is to be committed.
"""
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import golden_openmw  # noqa: E402
GOLDEN_TAG = re.compile(r"golden: (t[a-z]+)\t(.*)$")
PLACE_SLOP = 2.0
SPEED_SLOP = 0.5


def read_openmw(path):
    d = {"obj": defaultdict(list), "sv": defaultdict(list), "actor": defaultdict(list), "global": {}, "cells": set()}
    for ln in open(path, encoding="utf-8", errors="replace"):
        f = ln.rstrip("\n").split("\t")
        if f[0] == "cell":
            d["cells"].add(f[2])
        elif f[0] in ("obj", "sv", "actor"):
            d[f[0]][f[1]].append(f[2:])
        elif f[0] == "global":
            d["global"][f[2].lower()] = f[3]
    return d


def read_ours(path):
    d = {"obj": defaultdict(list), "sv": defaultdict(list), "actor": defaultdict(list), "global": {}, "cells": set()}
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = GOLDEN_TAG.search(ln.rstrip("\r\n"))
        if not m:
            continue
        f = m.group(2).split("\t")
        kind = m.group(1)
        if kind == "tcell":
            d["cells"].add(f[0])
        elif kind == "tobj":
            d["obj"][f[0]].append(f[1:])
        elif kind == "tsv":
            d["sv"][f[0]].append(f[1:])
        elif kind == "tactor":
            d["actor"][f[0]].append(f[1:])
        elif kind == "tglobal":
            d["global"][f[0].lower()] = f[1]
    return d


def xyz(text):
    return [float(v) for v in text.split(",")]


def near(a, b):
    return all(abs(p - q) <= PLACE_SLOP for p, q in zip(xyz(a), xyz(b)))


def obj_fields(o):
    """id, enabled, is actor, place, scale, count, lock, locked, trap, owner, faction, rank, all lowered"""
    o = [x.lower() for x in o] + [""] * 12
    return o[:12]


def compare_objects(cell, want, have, out):
    # (a creature OpenMW spawned from a leveled spot, its last field 0, is that spot's pick: dice; the north marker
    # OpenMW takes out on loading)
    def kept(o):
        return not o[0].lower().endswith("marker") and not o[0].lower().startswith("chargen")
    wf = [obj_fields(o) for o in want if not (len(o) > 12 and o[12] == "0") and kept(o)]
    hf = [obj_fields(o) for o in have if kept(o)]
    by_id = defaultdict(list)
    for h in hf:
        by_id[h[0]].append(h)
    names = ["enabled", "scale", "count", "lock", "locked", "trap", "owner", "faction", "rank"]
    slots = [1, 4, 5, 6, 7, 8, 9, 10, 11]
    for w in wf:
        pool = by_id[w[0]]
        match = None
        for h in pool:
            if (w[2] == "1" and h[2] == "1") or near(w[3], h[3]):
                match = h
                break
        if match is None:
            # moved rather than gone?
            out["obj missing" if not pool else "obj moved"].append((cell, w[0], w[3], pool[0][3] if pool else ""))
            continue
        pool.remove(match)
        for name, s in zip(names, slots):
            a, b = w[s], match[s]
            if name == "enabled" and match[2] == "2":      # (a leveled spot that came up empty: dice)
                continue
            if name == "scale" and abs(float(a or 1) - float(b or 1)) < 0.01:
                continue
            if name == "lock" and (w[7] != "1" and match[7] != "1"):
                continue
            if name == "lock" and b == "1000" and a == "0":        # (kKeyOnlyLock: a key and no level)
                continue
            if name == "rank":                 # (OpenMW's Lua counts ranks from 1, ours from 0)
                if w[10] and a and b and int(a) != int(b) + 1:
                    out["obj rank"].append((cell, w[0], w[3], "openmw " + a, "ours " + b))
                continue
            if a != b:
                out["obj " + name].append((cell, w[0], w[3], "openmw " + a, "ours " + b))
    for pool in by_id.values():
        for h in pool:
            out["obj extra"].append((cell, h[0], h[3]))


def vars_of(text):
    v = {}
    for part in text.split(";"):
        if "=" in part:
            k, n = part.split("=", 1)
            v[k.lower()] = float(n)
    return v


def compare_scripts(cell, want, have, placed, out):
    # (a leveled spot's pick is dice, and its script with it: only what was placed)
    by = defaultdict(list)
    for h in have:
        if h[0].lower() in placed:
            by[(h[0].lower(), h[1].lower())].append(h)
    for w in want:
        key = (w[0].lower(), w[1].lower())
        if key[0] not in placed:
            continue
        pool = by[key]
        if not pool:
            out["script not running on ours"].append((cell, *key))
            continue
        wv = vars_of(w[3] if len(w) > 3 else "")
        # the closest of those with this id and script
        best, bad = None, None
        for h in pool:
            hv = vars_of(h[3] if len(h) > 3 else "")
            diff = {k: (wv[k], hv.get(k)) for k in wv
                    if hv.get(k) is None or abs(wv[k] - hv[k]) > 0.001 * max(1.0, abs(wv[k]))}
            if bad is None or len(diff) < len(bad):
                best, bad = h, diff
        pool.remove(best)
        if w[2] != best[2]:
            out["script running flag"].append((cell, *key, "openmw " + w[2], "ours " + best[2]))
        for k, (a, b) in bad.items():
            # (a banner's die, and each side's clock)
            if key[1] == "outsidebanner" and k == "ran":
                continue
            timer = b is None or a != int(a) or b != int(b)
            out["script var (fraction)" if timer else "script var"].append((cell, *key, k, a, b))
    for key, pool in by.items():
        for _ in pool:
            out["script only on ours"].append((cell, *key))


ACTOR_FIELDS = ["package", "foes", "disposition", "ai settings", "pools", "speeds", "barter gold", "dead"]


def actor_row(a):
    """package, foes, disposition, fight/flee/alarm/hello, pools, run/walk, barter gold, dead (from either side)"""
    a = a + [""] * 12
    return {"id": a[0].lower(), "package": a[1], "foes": ",".join(sorted(x.lower() for x in a[2].split(",") if x)),
            "disposition": a[3], "ai settings": a[4], "pools": a[5], "speeds": a[6], "barter gold": a[7],
            "dead": a[8]}


def same(name, a, b):
    if name == "pools":                # (a body: ours keeps a maximum of 1)
        wa, ha = xyz(a), xyz(b)
        # (and a magicka of 241.5 is 242 there, 241 here)
        return wa[2] == ha[2] and abs(wa[1] - ha[1]) <= 1 and (wa[0] == ha[0] or (wa[0] == 0 and ha[0] == 1))
    if name == "speeds":
        return all(abs(x - y) <= SPEED_SLOP for x, y in zip(xyz(a), xyz(b)))
    if name == "walk":
        return abs(xyz(a)[1] - xyz(b)[1]) <= SPEED_SLOP
    return a == b


def compare_actors(cell, want, have, placed, out):
    w_by, h_by = defaultdict(list), defaultdict(list)
    for a in want:
        r = actor_row(a[:7] + a[8:])        # (OpenMW also gives capacity, not tracked here)
        w_by[r["id"]].append(r)
    for a in have:
        r = actor_row(a)
        h_by[r["id"]].append(r)
    for rid in sorted(set(w_by) | set(h_by)):
        ws, hs = w_by[rid], h_by[rid]
        if len(ws) != len(hs) and rid in placed:
            out["actor count"].append((cell, rid, len(ws), len(hs)))
        for w in ws:
            if not hs:
                break
            h = min(hs, key=lambda x: sum(not same(n, w[n], x[n]) for n in ACTOR_FIELDS))
            hs.remove(h)
            for n in ACTOR_FIELDS:
                # (OpenMW's creature run speed is its walk speed; ours follows the run animation, measured apart)
                if n == "speeds" and not w["disposition"] and same("walk", w[n], h[n]):
                    continue
                # (who has noticed the player is a roll, so a fight with the player says nothing about the package; a
                # standing NPC has a Wander of distance 0 in OpenMW, none in ours)
                if n in ("package", "foes"):
                    wf = [x for x in w["foes"].split(",") if x and x != "player"]
                    hf = [x for x in h["foes"].split(",") if x and x != "player"]
                    if n == "foes" and wf == hf:
                        continue
                    if n == "package" and (({w[n], h[n]} <= {"", "Wander"})
                                           or ("Combat" in (w[n], h[n]) and not wf and not hf)):
                        continue
                if not same(n, w[n], h[n]):
                    out["actor " + n].append((cell, rid, "openmw " + w[n], "ours " + h[n]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--log", type=Path, default=ROOT / "build" / "native-golden" / "sd" / "3ds" / "mw3ds" / "log.txt")
    ap.add_argument("--golden", type=Path, default=None, help="OpenMW's tour (default: build/golden/openmw-tour.txt, "
                    "openmw-tour-tribunal-bloodmoon.txt with the add-ons)")
    ap.add_argument("--show", type=int, default=12)
    ap.add_argument("--only", default="", help="only groups whose name holds this text")
    ap.add_argument("--json", type=Path, default=None, help="every difference (default: build/golden/tour-diff.json, "
                    "tour-diff-tribunal-bloodmoon.json with the add-ons)")
    golden_openmw.addon_args(ap)
    a = ap.parse_args()
    golden_openmw.set_addons(a)
    sfx = golden_openmw.suffix()
    a.golden = a.golden or ROOT / "build" / "golden" / f"openmw-tour{sfx}.txt"
    a.json = a.json or ROOT / "build" / "golden" / f"tour-diff{sfx}.json"
    want, have = read_openmw(a.golden), read_ours(a.log)
    both = want["cells"] & have["cells"]
    print(f"cells: openmw {len(want['cells'])}, ours {len(have['cells'])}, both {len(both)}")
    for c in sorted(want["cells"] - have["cells"])[:10]:
        print("  only openmw reached:", c)
    out = defaultdict(list)
    for cell in sorted(both):
        compare_objects(cell, want["obj"][cell], have["obj"][cell], out)
        placed = {o[0].lower() for o in want["obj"][cell] if not (len(o) > 12 and o[12] == "0")}
        compare_scripts(cell, want["sv"][cell], have["sv"][cell], placed, out)
        compare_actors(cell, want["actor"][cell], have["actor"][cell], placed, out)
    for k in sorted(set(want["global"]) & set(have["global"])):
        # (the clock; PCHasCrimeGold: OpenMW only works it out for a talk)
        if k not in ("gamehour", "pchascrimegold", "pchasgolddiscount", "pchasturnin", "random100") and float(want["global"][k]) != float(have["global"][k]):
            out["global"].append((k, want["global"][k], have["global"][k]))
    a.json.write_text(json.dumps(out, indent=0))
    for name, rows in sorted(out.items(), key=lambda kv: -len(kv[1])):
        if a.only not in name:
            continue
        print(f"{name}: {len(rows)} in {len({r[0] for r in rows})} cells")
        for r in rows[:a.show]:
            print("   ", " | ".join(str(x) for x in r)[:150])
    print(f"{sum(len(v) for v in out.values())} differences; all in {a.json}")
    print("by kind:", dict(Counter({k: len(v) for k, v in out.items()}).most_common()))


if __name__ == "__main__":
    main()
