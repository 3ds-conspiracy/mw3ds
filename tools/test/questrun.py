"""Side-quest sweep: a harness test per batch of quests that tries each quest's opening line in the game.

For every quest, the dialogue response that sets its lowest stage from a named speaker is the opener.
Its conditions are met the way the harness can (other quests' journal stages, globals, items, the PC
faction and rank the response asks for, level), then the player goes to the speaker (GOTO an interior,
TP outdoors), talks (TALK: disposition 100), picks the topic (and the choice the response asks for),
and the journal is checked.

  python tools/test/questrun.py [--filter REGEX] [--batch 30]
Writes tools/test/cases/qrun-<k>.txt and build/qrun.json ({test: [[quest, stage, why-skipped-or-""]...]});
  python tools/test/questrun.py --check qrun-3      reads the emulator log: which quests started
Run a batch: tools\\test\\run-test.ps1 qrun-3 -Data out\\world -Start "Seyda Neen" -Wait 1800
"""
import argparse
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from envfile import require
from mwfiles import DATA_FILES, cell_refs, load_db, read_records
from questcheck import JOURNAL_SET

ROOT = Path(__file__).resolve().parents[2]
LOG = Path(require("MW3DS_EMU_DIR", "the Azahar folder")) / "user" / "sdmc" / "3ds" / "mw3ds" / "log.txt"
OPS = {"0": "=", "1": "!=", "2": ">", "3": ">=", "4": "<", "5": "<="}


def conds(info):
    """[(type char, function code, op, name, value)] of a response's conditions."""
    out, subs = [], info.subs
    for i, (t, d) in enumerate(subs):
        if t != "SCVR":
            continue
        s = d.decode("latin-1")
        val = 0
        if i + 1 < len(subs) and subs[i + 1][0] in ("INTV", "FLTV"):
            nd = subs[i + 1][1]
            val = struct.unpack("<i", nd)[0] if subs[i + 1][0] == "INTV" else struct.unpack("<f", nd)[0]
        out.append((s[1], s[2:4], OPS.get(s[4], "="), s[5:], val))
    return out


def meet(op, v):
    """A value that satisfies `x op v` (None when 0 already does)."""
    if op in ("=", ">="):
        return v if v != 0 else None
    if op == ">":
        return v + 1
    if op in ("<", "<=", "!="):
        return None if (op != "<=" or v >= 0) and (op != "<" or v > 0) and (op != "!=" or v != 0) else v - 1
    return None


def exact(op, v):
    """A value that satisfies `x op v`."""
    return {"=": v, ">=": v, ">": v + 1, "<": v - 1, "<=": v, "!=": v + 1}.get(op, v)


def choice_parents(dial, info, cs, depth=0):
    """The chain of choices leading to a response that needs Choice = k: [(parent conds, k), ...] from the
    first line said to the last choice. None when no parent offers it."""
    ks = [int(val) for t, fn, op, n, val in cs if t == "1" and fn == "50"]
    if not ks:
        return []
    if depth > 3:
        return None
    k = ks[0]
    offer = re.compile(r'(?im)^\s*choice\b.*"[^"]*"\s*,?\s*%d\b' % k)
    # the offer that fits best: the one testing the same journals as the answer (one speaker often asks
    # the same question for several quests: "Have you brought me the marshmerrow / the netch leather?")
    mine = {n.lower() for t, _, _, n, _ in cs if t == "4"}
    found = []
    for other in dial["infos"]:
        if other is info or not offer.search(other.zstr("BNAM") or ""):
            continue
        if other.zstr("ONAM") and info.zstr("ONAM") and other.zstr("ONAM").lower() != info.zstr("ONAM").lower():
            continue
        pcs = conds(other)
        up = choice_parents(dial, other, pcs, depth + 1)
        if up is None:
            continue
        shared = len(mine & {n.lower() for t, _, _, n, _ in pcs if t == "4"})
        found.append((-shared, len(found), up + [(pcs, k, other)]))
    return min(found, key=lambda f: (f[0], f[1]))[2] if found else None


def token(s):
    # spaces become underscores for the harness; an id with both ("bk_Boethiah's Glory_unique") keeps its
    # underscores and spells its spaces '%'
    return s.replace(" ", "%") if "_" in s and " " in s else s.replace(" ", "_")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--batch", type=int, default=30)
    ap.add_argument("--check")
    ap.add_argument("--log", help="the emulator log to check (default: the main Azahar copy's)")
    ap.add_argument("--stages", action="store_true",
                    help="every later stage a response sets (its quest's earlier stage set first): srun-N batches")
    args = ap.parse_args()
    prefix = "srun" if args.stages else "qrun"
    if args.check and args.check.startswith("chain-"):
        from questchain import check as chain_check
        return chain_check(args.check, Path(args.log) if args.log else LOG)
    if args.check:
        return check(args.check, Path(args.log) if args.log else LOG)
    db = load_db()
    want = re.compile(args.filter, re.I)

    where = {}                                   # id -> ("int", cell name) | ("ext", pos)
    for key, cell in db["CELL"].items():
        for r in cell_refs(cell):
            if r["deleted"] or not r["id"] or not r["pos"]:
                continue
            rid = r["id"].lower()
            if rid in where:
                continue
            where[rid] = ("int", cell.zstr("NAME")) if isinstance(key, str) else ("ext", r["pos"], r["rot"])
    factions = {}
    for fid, f in db.get("FACT", {}).items():
        factions[fid.lower()] = f.zstr("FNAM") or fid

    quests = {d["name"].lower() for d in db["dialogue"] if d["type"] == 4}
    # People and things Startup hides until their quests bring them in
    hidden = {m.lower() for m in re.findall(r'(?im)^\s*"?([^"\n]+?)"?\s*->\s*disable', db["SCPT"].get("startup", ""))}
    openers = {}
    for dial in db["dialogue"]:
        if dial["type"] not in (0, 2):           # topics, greetings
            continue
        for info in dial["infos"]:
            who = info.zstr("ONAM")
            if not who:
                continue
            for q, v in JOURNAL_SET.findall(info.zstr("BNAM") or ""):
                q, v = q.lower(), int(v)
                if q not in quests or not want.search(q) or v <= 0:
                    continue
                cs = conds(info)
                # an opener: no stage of its own quest needed already (--stages: only the others)
                later = any(t == "4" and n.lower() == q and meet(op, val) for t, _, op, n, val in cs)
                if later != args.stages:
                    continue
                chain = choice_parents(dial, info, cs)
                if chain is None:
                    continue                  # a choice nothing offers
                # the parents' conditions have to hold too (they are said first)
                allc = [c for c in cs if not (c[0] == "1" and c[1] == "50")]
                for pcs, _, _ in chain:
                    allc += [c for c in pcs if not (c[0] == "1" and c[1] == "50")]
                # journals a parent's own result sets (the offer writes stage 1, then asks) are not set
                # beforehand: that would hide the offer itself
                by_parents = {jq.lower() for _, _, par in chain for jq, _ in JOURNAL_SET.findall(par.zstr("BNAM") or "")}
                allc = [c for c in allc if not (c[0] == "4" and c[3].lower() in by_parents)]
                choices = [k for _, k, _ in chain]
                key = (q, v) if args.stages else q
                better = key not in openers or v < openers[key][1] or (v == openers[key][1] and len(choices) < len(openers[key][5]))
                if better:
                    openers[key] = (dial, v, info, allc, who, choices, by_parents, [par for _, _, par in chain])

    # globals a quest's conditions need are put back after it (pcvampire left at 1 made every later
    # NPC in the batch curse the player and turn hostile)
    glob_default = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"GLOB"}):
        fl = r.get("FLTV")
        glob_default[r.id.lower()] = struct.unpack("<f", fl[:4])[0] if fl and len(fl) >= 4 else 0.0
    tests, plan = [], {}
    rows = sorted(openers.items(), key=lambda kv: str(where.get(kv[1][4].lower(), ("~", ""))[1]))
    for k in range(0, len(rows), args.batch):
        name = f"{prefix}-{k // args.batch + 1}"
        lines = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 BOOST:100", "0.3 0 0 0 0 EFFECT:11:100:99999",
                 f"1 0 0 0 0 SAVE:{prefix}"]
        entries = []
        for key, (dial, v, info, cs, who, choices, by_parents, parents) in rows[k:k + args.batch]:
            q = key[0] if args.stages else key
            skip = ""
            steps = []
            level = 30
            for t, fn, op, n, val in cs:
                if t == "4" and (n.lower() != q or args.stages):
                    m = meet(op, val)
                    if m is not None:
                        steps.append(f"JOURNAL:{token(n)}:{int(m)}")
                elif t == "2":
                    m = meet(op, val)
                    if m is not None:
                        steps.append(f"SETGLOBAL:{n.lower()}:{m}")
                elif t == "5":
                    m = meet(op, val)
                    if m is not None and m > 0:
                        steps.append(f"GIVE:{token(n)}:{int(m)}")
                elif t == "6" and meet(op, val):
                    skip = f"needs {n} dead"
                elif t == "3":
                    m = exact(op, val)
                    steps.append(f"SETLOCAL:{token(who)}:{n.lower()}:{m}")
                elif t == "1" and fn == "60" and exact(op, val) > 0:
                    steps.append("SETGLOBAL:pcvampire:1")
                elif t == "1" and fn == "05":
                    steps.append(f"REP:{max(0, int(exact(op, val)))}")
                elif t == "1" and fn == "64" and op in (">=", ">", "="):
                    steps.append(f"EFFECT:80:{int(exact(op, val))}:99999")   # the PC's health: Fortify Health
                elif t == "1" and fn == "06":
                    level = max(1, int(exact(op, val)))
            # the PC's faction and rank the answer and the offers before it ask for (the highest)
            need = {}
            for inf in [info] + parents:
                dnam, pcrank = inf.zstr("DNAM"), inf.get("DATA")[10] if inf.get("DATA") else 255
                if dnam and dnam.lower() in factions:
                    need[dnam.lower()] = max(need.get(dnam.lower(), 0), pcrank if pcrank < 128 else 0)
            for f, r in need.items():
                steps.append(f"JOIN:{token(f)}:{r}")
            w = where.get(who.lower())
            if w is None:
                skip = skip or "speaker placed by a script"
            if who.lower() in hidden:
                skip = skip or "speaker hidden by Startup until another quest"
            # one journal asked for by the answer and its offers (">= 20" and ">= 1"): the highest
            top = {}
            for st in steps:
                if st.startswith("JOURNAL:"):
                    _, jn, jv = st.split(":")
                    top[jn.lower()] = max(top.get(jn.lower(), -1), int(jv))
            steps = [st for st in steps if not st.startswith("JOURNAL:") or int(st.split(":")[2]) == top[st.split(":")[1].lower()]]
            steps = list(dict.fromkeys(steps))        # the answer and its offer often ask the same
            if skip:
                entries.append([q, v, skip])
                continue
            lines += [f"0.5 0 0 0 0 LOAD:{prefix}", "3 0 0 0"]
            lines.append(f"0.3 0 0 0 0 LEVEL:{level}")
            for s in steps:
                if not s.startswith("SETLOCAL:"):
                    lines.append("0.3 0 0 0 0 " + s)
            if w[0] == "int":
                lines.append("0.3 0 0 0 0 GOTO:" + token(w[1]))
            else:
                x, y, z = w[1]
                lines.append(f"0.3 0 0 0 0 TP:{int(x)}:{int(y + 200)}:{int(z + 300)}:180")
            lines.append("8 0 0 0")
            # script locals once the speaker is loaded (set before, there was no instance to set them in)
            lines += ["0.3 0 0 0 0 " + s for s in steps if s.startswith("SETLOCAL:")]
            lines += ["0.3 0 0 0 0 TALK:" + token(who), "1.5 0 0 0"]
            if dial["type"] == 0:
                lines += ["0.3 0 0 0 0 TOPIC:" + token(dial["name"]), "1.5 0 0 0"]
            for c in choices:
                lines += [f"0.3 0 0 0 0 CHOICEVAL:{c}", "1.5 0 0 0"]
            lines += ["2 0 0 0 0 MSG", f"0.3 0 0 0 0 CHECK:journal:{token(q)}", "0.3 0 0 0 0 B", "0.5 0 0 0 0 B",
                      "1 0 0 0"]
            entries.append([q, v, ""])
        (ROOT / "tools" / "test" / "cases" / f"{name}.txt").write_text("\n".join(lines) + "\n")
        plan[name] = entries
        tests.append(name)
    (ROOT / "build" / f"{prefix}.json").write_text(json.dumps(plan, indent=1))
    total = sum(len(e) for e in plan.values())
    skipped = sum(1 for e in plan.values() for x in e if x[2])
    what = "later stages set in talk" if args.stages else "quests with an opener"
    print(f"{len(openers)} {what}, {total - skipped} to run in {len(tests)} batches, {skipped} skipped")


def check(name, log=LOG):
    plan = json.loads((ROOT / "build" / (name.split("-")[0] + ".json")).read_text())[name]
    seen = defaultdict(list)
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"check: journal (\S+) = (-?\d+)", line)
        if m:
            seen[m.group(1)].append(int(m.group(2)))
    ok = bad = 0
    for q, v, skip in plan:
        if skip:
            print(f"  skip  {q}: {skip}")
            continue
        vals = seen.get(q.replace(" ", "_").lower()) or seen.get(q.lower()) or []
        got = vals.pop(0) if vals else None
        if got is not None and got >= v:
            ok += 1
        else:
            bad += 1
            print(f"  FAIL  {q}: wanted {v}, got {got}")
            continue
    print(f"{name}: {ok} started, {bad} not")


if __name__ == "__main__":
    main()
