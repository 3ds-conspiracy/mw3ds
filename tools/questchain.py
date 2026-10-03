"""Chained quest runs: each quest played from its opener to its last dialogue-set stage, in one go.

questrun.py --stages tests every later stage from a warped state (the quest's earlier stage set by the test).
That can't see a chain that breaks: a stage whose response needs the journal a previous response should have
left. Here a quest's own journal is never set. Each stage's other conditions (items, globals, other quests'
journals, faction and rank) are set up, then the player talks to the speaker, and the journal is checked
before the next stage begins, so each stage is reached by really playing the one before it.

The stage order is worked out from the responses' own-quest conditions: from index 0, the response with the
lowest stage above the current one whose conditions the current index meets. Stages nothing can reach that
way (a script sets the step in between: xrun's) are listed as "unreachable"; the chain ends there.

  python tools/questchain.py [--filter REGEX] [--batch 6]     -> tools/tests/chain-N.txt, build/chain.json
  python tools/questchain.py --list [--filter REGEX]          the stage order per quest, no files written
  python tools/questrun.py --check chain-3 [--log F]          per quest: the first stage where the chain breaks
Run: tools\\sweep-par.ps1 -ItemsFile <file of chain-N lines>  (or tools\\run-test.ps1 chain-3 ...)
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mwfiles import DATA_FILES, cell_refs, load_db, read_records  # noqa: E402
from questcheck import JOURNAL_SET  # noqa: E402
from questrun import ROOT, LOG, choice_parents, conds, token  # noqa: E402
import struct  # noqa: E402

PREFIX = "chain"


def holds(op, have, want):
    return {"=": have == want, "!=": have != want, ">": have > want, ">=": have >= want,
            "<": have < want, "<=": have <= want}[op]


def satisfying(op, v):
    """A value that satisfies `x op v` (for a condition on another quest, a global, a count)."""
    return {"=": v, ">=": v, ">": v + 1, "<": v - 1, "<=": v, "!=": v + 1}.get(op, v)


def collect(db, want):
    """{quest: [candidate]}: every response by a named speaker that sets a stage of a quest. A candidate is
    (stage, dial, info, all conditions, speaker, choices, parents)."""
    quests = {d["name"].lower() for d in db["dialogue"] if d["type"] == 4}
    out = defaultdict(list)
    for dial in db["dialogue"]:
        if dial["type"] not in (0, 2):                       # topics, greetings
            continue
        for info in dial["infos"]:
            who = info.zstr("ONAM")
            if not who:
                continue
            sets = [(q.lower(), int(v)) for q, v in JOURNAL_SET.findall(info.zstr("BNAM") or "")]
            if not any(q in quests and want.search(q) and v > 0 for q, v in sets):
                continue
            cs = conds(info)
            chain = choice_parents(dial, info, cs)
            if chain is None:
                continue                                      # a choice nothing offers
            allc = [c for c in cs if not (c[0] == "1" and c[1] == "50")]
            for pcs, _, _ in chain:
                allc += [c for c in pcs if not (c[0] == "1" and c[1] == "50")]
            by_parents = {jq.lower() for _, _, par in chain for jq, _ in JOURNAL_SET.findall(par.zstr("BNAM") or "")}
            # what a parent's own result sets is not asked of the test: the offer writes it itself
            allc = [c for c in allc if not (c[0] == "4" and c[3].lower() in by_parents)]
            for q, v in sets:
                if q in quests and want.search(q) and v > 0:
                    out[q].append((v, dial, info, allc, who, [k for _, k, _ in chain], [par for _, _, par in chain]))
    return out


def order(cands):
    """The stages in the order they can be played, from journal index 0. Returns (played, unreachable)
    where played is [candidate] and unreachable the stages left over."""
    cur, played, left = 0, [], list(cands)
    taken = {}                       # an offer's answer already played: its other answers are alternatives
    while True:
        ok = []
        for c in left:
            v, cs = c[0], c[3]
            if v <= cur:
                continue
            if any(id(par) in taken and taken[id(par)] != k for par, k in zip(c[6], c[5])):
                continue
            own = [(op, val) for t, fn, op, n, val in cs if t == "4" and n.lower() == q_of(c)]
            if all(holds(op, cur, int(val)) for op, val in own):
                ok.append(c)
        if not ok:
            return played, sorted({c[0] for c in left if c[0] > cur})
        best = min(ok, key=lambda c: (c[0], len(c[5])))
        for par, k in zip(best[6], best[5]):
            taken[id(par)] = k
        played.append(best)
        cur = best[0]
        left = [c for c in left if c[0] > cur]


_q = {}


def q_of(c):
    return _q[id(c)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--batch", type=int, default=6)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    db = load_db()
    want = re.compile(args.filter, re.I)
    cands = collect(db, want)
    for q, cl in cands.items():
        for c in cl:
            _q[id(c)] = q

    where = {}
    for key, cell in db["CELL"].items():
        for r in cell_refs(cell):
            if r["deleted"] or not r["id"] or not r["pos"]:
                continue
            rid = r["id"].lower()
            if rid not in where:
                where[rid] = ("int", cell.zstr("NAME")) if isinstance(key, str) else ("ext", r["pos"], r["rot"])
    factions = {fid.lower(): f.zstr("FNAM") or fid for fid, f in db.get("FACT", {}).items()}
    hidden = {m.lower() for m in re.findall(r'(?im)^\s*"?([^"\n]+?)"?\s*->\s*disable', db["SCPT"].get("startup", ""))}

    plans = {}
    for q, cl in sorted(cands.items()):
        played, unreachable = order(cl)
        # the chain ends at the first stage nothing in the game can play (speaker placed by a script ...)
        run, why = [], ""
        for c in played:
            who = c[4].lower()
            if who not in where:
                why = f"stage {c[0]}: speaker {c[4]} placed by a script"
                break
            if who in hidden:
                why = f"stage {c[0]}: speaker {c[4]} hidden by Startup until another quest"
                break
            dead = [n for t, fn, op, n, val in c[3] if t == "6" and satisfying(op, val) > 0]
            if any(d.lower() not in where or d.lower() == who or d.lower() in hidden for d in dead):
                why = f"stage {c[0]}: needs {dead} dead (not a person the player can reach)"
                break
            run.append(c)
        if not run:
            plans[q] = ([], why or "no opener", unreachable)
        else:
            plans[q] = (run, why, unreachable if not why else unreachable)
    if args.list:
        for q, (run, why, unreachable) in plans.items():
            print(f"{q}: {[c[0] for c in run]}" + (f"  ends: {why}" if why else "") +
                  (f"  no dialogue reaches: {unreachable}" if unreachable else ""))
        return

    glob_default = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"GLOB"}):
        fl = r.get("FLTV")
        glob_default[r.id.lower()] = struct.unpack("<f", fl[:4])[0] if fl and len(fl) >= 4 else 0.0

    rows = [(q, p) for q, p in plans.items() if p[0]]
    rows.sort(key=lambda kv: str(where.get(kv[1][0][0][4].lower(), ("~", ""))[1]))
    plan, tests = {}, []
    for k in range(0, len(rows), args.batch):
        name = f"{PREFIX}-{k // args.batch + 1}"
        lines = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 BOOST:100", "0.3 0 0 0 0 EFFECT:11:100:99999", f"1 0 0 0 0 SAVE:{PREFIX}"]
        entries = []
        for q, (run, why, unreachable) in rows[k:k + args.batch]:
            lines += [f"0.5 0 0 0 0 LOAD:{PREFIX}", "3 0 0 0"]
            level = 30
            for c in run:
                for t, fn, op, n, val in c[3]:
                    if t == "1" and fn == "06":               # the player's level: a low one when asked for ("<= 5")
                        sat = max(1, satisfying(op, int(val)))
                        level = min(level, sat) if op in ("<", "<=") else max(level, sat)
            lines.append(f"0.3 0 0 0 0 LEVEL:{level}")
            last_spot = None
            prev_c, open_conv = None, False
            for c in run:
                v, dial, info, cs, who, choices, parents = c
                # the answer to the offer that the previous stage's own response made: same conversation
                cont = (open_conv and prev_c is not None and bool(choices) and any(prev_c[2] is par for par in parents)
                        and not any(t == "6" and satisfying(op, val) > 0 for t, fn, op, n, val in cs))
                if open_conv and not cont:
                    lines += ["0.3 0 0 0 0 B", "0.5 0 0 0 0 B", "1 0 0 0"]
                    open_conv = False
                steps = []
                for t, fn, op, n, val in cs:
                    if t == "4" and n.lower() != q:            # the quest's own journal is never set
                        if op in ("=", ">=", ">") or (op == "!=" and val == 0):
                            steps.append(f"JOURNAL:{token(n)}:{int(satisfying(op, val))}")
                    elif t == "2" and op in ("=", ">=", ">", "!="):
                        steps.append(f"SETGLOBAL:{n.lower()}:{satisfying(op, val)}")
                    elif t == "5" and op in ("=", ">=", ">"):
                        m = satisfying(op, val)
                        if m > 0:
                            steps.append(f"GIVE:{token(n)}:{int(m)}")
                    elif t == "3":
                        steps.append(f"SETLOCAL:{token(who)}:{n.lower()}:{satisfying(op, val)}")
                    elif t == "1" and fn == "60" and satisfying(op, val) > 0:
                        steps.append("SETGLOBAL:pcvampire:1")
                    elif t == "1" and fn == "05":
                        steps.append(f"REP:{max(0, int(satisfying(op, val)))}")
                    elif t == "1" and fn == "64" and op in (">=", ">", "="):
                        steps.append(f"EFFECT:80:{int(satisfying(op, val))}:99999")
                need = {}
                for inf in [info] + parents:
                    dnam, pcrank = inf.zstr("DNAM"), inf.get("DATA")[10] if inf.get("DATA") else 255
                    if dnam and dnam.lower() in factions:
                        need[dnam.lower()] = max(need.get(dnam.lower(), 0), pcrank if pcrank < 128 else 0)
                for f, r in need.items():
                    steps.append(f"JOIN:{token(f)}:{r}")
                top = {}
                for st in steps:
                    if st.startswith("JOURNAL:"):
                        _, jn, jv = st.split(":")
                        top[jn.lower()] = max(top.get(jn.lower(), -1), int(jv))
                steps = [st for st in steps if not st.startswith("JOURNAL:") or int(st.split(":")[2]) == top[st.split(":")[1].lower()]]
                steps = list(dict.fromkeys(steps))
                lines += ["0.3 0 0 0 0 " + s for s in steps if not s.startswith("SETLOCAL:")]
                if cont:
                    lines += [f"0.3 0 0 0 0 CHOICEVAL:{choices[-1]}", "1.5 0 0 0", "2 0 0 0 0 MSG",
                              f"0.3 0 0 0 0 CHECK:journal:{token(q)}"]
                    prev_c = c
                    continue
                # somebody has to be dead first (GetDeadCount): the player kills them, as the story would
                dead_first = [n for t, fn, op, n, val in cs if t == "6" and satisfying(op, val) > 0]
                for d in dead_first:
                    dw = where[d.lower()]
                    if dw[0] == "int":
                        lines.append("0.3 0 0 0 0 GOTO:" + token(dw[1]))
                    else:
                        dx, dy, dz = dw[1]
                        lines.append(f"0.3 0 0 0 0 TP:{int(dx)}:{int(dy + 200)}:{int(dz + 300)}:180")
                    # the death is what the response asks for, not a fight: an instant kill (the driver's walk to
                    # the target stalls on stairs in the ruins)
                    lines += ["8 0 0 0", "0.3 0 0 0 0 SOULKILL:" + token(d), "1 0 0 0"]
                    last_spot = None
                w = where[who.lower()]
                spot = (who.lower(), bool(dead_first))
                if spot != last_spot:                          # still standing at them from the last stage: no trip
                    if w[0] == "int":
                        lines.append("0.3 0 0 0 0 GOTO:" + token(w[1]))
                    else:
                        x, y, z = w[1]
                        lines.append(f"0.3 0 0 0 0 TP:{int(x)}:{int(y + 200)}:{int(z + 300)}:180")
                    lines.append("8 0 0 0")
                last_spot = spot
                lines += ["0.3 0 0 0 0 " + s for s in steps if s.startswith("SETLOCAL:")]
                lines += ["0.3 0 0 0 0 TALK:" + token(who), "1.5 0 0 0"]
                if dial["type"] == 0:
                    lines += ["0.3 0 0 0 0 TOPIC:" + token(dial["name"]), "1.5 0 0 0"]
                for ch in choices:
                    lines += [f"0.3 0 0 0 0 CHOICEVAL:{ch}", "1.5 0 0 0"]
                lines += ["2 0 0 0 0 MSG", f"0.3 0 0 0 0 CHECK:journal:{token(q)}"]
                open_conv = True
                prev_c = c
            if open_conv:
                lines += ["0.3 0 0 0 0 B", "0.5 0 0 0 0 B", "1 0 0 0"]
            entries.append([q, [c[0] for c in run], why, unreachable])
        (ROOT / "tools" / "tests" / f"{name}.txt").write_text("\n".join(lines) + "\n")
        plan[name] = entries
        tests.append(name)
    (ROOT / "build" / f"{PREFIX}.json").write_text(json.dumps(plan, indent=1))
    stages = sum(len(e[1]) for es in plan.values() for e in es)
    print(f"{len(rows)} quests, {stages} stages to play in {len(tests)} batches; "
          f"{sum(1 for e in plans.values() if not e[0])} quests with no playable opener")


def check(name, log=LOG):
    """Per quest: every stage reached, or the first stage where the chain broke."""
    plan = json.loads((ROOT / "build" / f"{PREFIX}.json").read_text())[name]
    seen = defaultdict(list)
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"check: journal (\S+) = (-?\d+)", line)
        if m:
            seen[m.group(1)].append(int(m.group(2)))
    whole = broke = 0
    for q, stages, why, unreachable in plan:
        vals = seen.get(q.replace(" ", "_").lower()) or seen.get(q.lower()) or []
        first = None
        for i, v in enumerate(stages):
            got = vals[i] if i < len(vals) else None
            if got is None or got < v:
                first = (i, v, got)
                break
        tail = f" (chain ends: {why})" if why else ""
        if first is None:
            whole += 1
            print(f"  ok    {q}: {stages}{tail}")
        else:
            broke += 1
            i, v, got = first
            print(f"  BREAK {q}: stage {v} (#{i + 1} of {stages}) wanted {v}, got {got}{tail}")
    print(f"{name}: {whole} chains played through, {broke} broke")


if __name__ == "__main__":
    main()
