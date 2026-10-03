"""Tests for the journal stages scripts set (deaths, activations, arriving somewhere, picking things up...), as
real actions: for each 'Journal Q n' in a script, the conditions around it become the setup and the action.

  object script, OnDeath             -> KILL the object (a ref of it)
  OnActivate                         -> ACTIVATE it
  OnPCAdd / Player->GetItemCount X   -> PICKUP it (the script's own item) / GIVE X
  OnPCEquip                          -> GIVE + EQUIP it
  GetDistance Player < n             -> WALKTO it
  GetPCCell "x"                      -> GOTO that cell
  GetDeadCount X >= 1                -> KILL X
  GetJournalIndex Q op v             -> JOURNAL setup     (globals: SETGLOBAL; GameHour: HOUR; GetPCSleep: SLEEP)
Anything else a stage depends on (script locals set elsewhere, positions, spells...) and it's skipped, with why.

  python tools/scriptrun.py [--filter re] [--batch 10]  -> tools/tests/xrun-N.txt, build/xrun.json
  tools\\qrun-all.ps1 -Prefix xrun -From 1 -To N; results: questrun.py --check xrun-N
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mwfiles import DATA_FILES, cell_refs, load_db, read_records  # noqa: E402
import mwscript  # noqa: E402
from questrun import meet, token, exact  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OPS = {"==": "=", "!=": "!=", ">": ">", ">=": ">=", "<": "<", "<=": "<="}


def calls(node, out):
    """Every call node in an expression."""
    if isinstance(node, list) and node:
        if node[0] == "c":
            out.append(node)
        for k in node[1:]:
            calls(k, out)
    return out


def has_journal(node):
    if isinstance(node, list):
        if node[:1] == ["c"] and len(node) > 2 and node[2] == "journal":
            return True
        return any(has_journal(k) for k in node)
    if isinstance(node, tuple):
        return any(has_journal(k) for k in node)
    return False


def ends_in_return(block):
    return bool(block) and isinstance(block[-1], list) and block[-1][:1] == ["ret"]


def journal_sites(body, conds=()):
    """(quest, index, [condition expressions around it]) for each Journal call in a script body. An
    'if (x) ... return endif' before it is a guard: what follows runs only when x is false."""
    for st in body:
        if not isinstance(st, list) or not st:
            continue
        if st[0] == "if" and all(ends_in_return(b) for _, b in st[1]) and (st[2] is None or ends_in_return(st[2]))                 and not has_journal(st):
            conds = conds + tuple(("not", c) for c, _ in st[1])
            continue
        if st[0] == "if":
            prior = []
            for cond, block in st[1]:
                yield from journal_sites(block, conds + tuple(prior) + (("is", cond),))
                prior.append(("not", cond))
            if st[2]:
                yield from journal_sites(st[2], conds + tuple(prior))
        elif st[0] == "while":
            yield from journal_sites(st[2] if len(st) > 2 else [], conds + (("is", st[1]),))
        elif st[0] == "c" and st[2] == "journal" and len(st[3]) >= 2:
            q, v = st[3][0], st[3][1]
            if isinstance(q, list) and q[0] == "s" and isinstance(v, (int, float)):
                yield q[1].lower(), int(v), list(conds)


def arg_str(a):
    return a[1] if isinstance(a, list) and a and a[0] == "s" else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--batch", type=int, default=10)
    args = ap.parse_args()
    want = re.compile(args.filter, re.I)
    db = load_db()
    globals_ = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"GLOB"}):
        globals_[r.id.lower()] = (r.zstr("FNAM") or "f")
    quests = {d["name"].lower() for d in db["dialogue"] if d["type"] == 4}
    dialogue_set = set()
    from questcheck import JOURNAL_SET
    for d in db["dialogue"]:
        for i in d["infos"]:
            for q, v in JOURNAL_SET.findall(i.zstr("BNAM") or ""):
                dialogue_set.add((q.lower(), int(v)))
    # who carries each script, and where those are
    owners = defaultdict(list)                      # script -> [object ids]
    tags = {}
    for oid, rec in db["objects"].items():
        sc = (rec.zstr("SCRI") or "").lower()
        if sc:
            owners[sc].append(oid)
            tags[oid] = rec.tag
    where = {}                                      # object id -> ("int", cell) | ("ext", pos)
    for key, cell in db["CELL"].items():
        for r in cell_refs(cell):
            rid = (r["id"] or "").lower()
            if rid and r["pos"] and rid not in where and not r["deleted"]:
                where[rid] = ("int", cell.zstr("NAME")) if isinstance(key, str) else ("ext", r["pos"])
    started = set()
    for text in list(db["SCPT"].values()) + [i.zstr("BNAM") or "" for d in db["dialogue"] for i in d["infos"]]:
        for m in re.finditer(r'(?i)startscript\W+"?([\w ]+?)"?\s*$', text, re.M):
            started.add(m.group(1).strip().lower())

    tests = []                                      # (quest, index, steps) | (quest, index, None, why)
    for name, text in db["SCPT"].items():
        try:
            tree = mwscript.compile_script(text, globals_)
        except Exception:
            continue
        for q, v, conds in journal_sites(tree["body"]):
            if q not in quests or not want.search(q) or (q, v) in dialogue_set or v <= 0:
                continue
            owner = next((o for o in owners.get(name, []) if o in where), None)
            steps, action, why = [], None, None
            follow, away = False, None
            cellreq = []                       # [(cell, must the player be in it)] from GetPCCell conditions
            inv = {"<": ">=", "<=": ">", ">": "<=", ">=": "<", "==": "!=", "!=": "=="}
            for kind, expr in conds:
                # a script local compared with a number (a flag another script or a dialogue result sets):
                # the value that makes the branch run, set on the owner once it is loaded (0 already does most)
                cmp_ = expr if kind == "is" else ([inv[expr[0]], expr[1], expr[2]] if isinstance(expr, list)
                                                 and len(expr) == 3 and expr[0] in inv else None)
                if (isinstance(cmp_, list) and len(cmp_) == 3 and cmp_[0] in OPS and isinstance(cmp_[1], list)
                        and cmp_[1][:1] == ["l"] and isinstance(cmp_[2], (int, float)) and owner):
                    ov = exact(OPS[cmp_[0]], cmp_[2])
                    if not {"=": 0 == cmp_[2], "!=": 0 != cmp_[2], ">": 0 > cmp_[2], ">=": 0 >= cmp_[2],
                            "<": 0 < cmp_[2], "<=": 0 <= cmp_[2]}[OPS[cmp_[0]]]:
                        steps.append(f"SETLOCAL:{token(owner)}:{cmp_[1][1]}:{int(ov)}")
                    continue
                # GetPCCell in a guard turned round: where the player has to be (or not be) for the branch to run
                if (isinstance(cmp_, list) and len(cmp_) == 3 and cmp_[0] in OPS and isinstance(cmp_[1], list)
                        and cmp_[1][:1] == ["c"] and cmp_[1][2] == "getpccell" and arg_str(cmp_[1][3][0] if cmp_[1][3] else None)
                        and isinstance(cmp_[2], (int, float))):
                    hold_in = {"=": cmp_[2] == 1, "!=": cmp_[2] != 1, ">": 1 > cmp_[2], ">=": 1 >= cmp_[2],
                               "<": 1 < cmp_[2], "<=": 1 <= cmp_[2]}[OPS[cmp_[0]]]
                    cellreq.append((arg_str(cmp_[1][3][0]), hold_in))
                    if kind == "not":
                        continue
                if kind == "not":
                    # a branch not taken / a guard: a journal or global comparison, turned round, is setup;
                    # anything else (MenuMode, OnActivate ...) needs nothing
                    inv = {"<": ">=", "<=": ">", ">": "<=", ">=": "<", "==": "!=", "!=": "=="}
                    lhs = expr[1] if isinstance(expr, list) and len(expr) == 3 and expr[0] in inv else None
                    fn = lhs[2] if isinstance(lhs, list) and lhs[:1] == ["c"] else None
                    if fn != "getjournalindex" and fn not in globals_:
                        continue
                    expr = [inv[expr[0]], expr[1], expr[2]]
                cs = calls(expr, [])
                op = expr[0] if isinstance(expr, list) and expr and expr[0] in OPS else None
                val = expr[2] if op and isinstance(expr[2], (int, float)) else 1
                for c in cs:
                    fn, ref, a = c[2], c[1], c[3]
                    if fn == "getcurrentaipackage":
                        # following the player (AiFollow, package 3) when the condition holds at 3: the escort
                        # the dialogue would have started
                        if owner and op and {"=": val == 3, "!=": val != 3, ">": 3 > val, ">=": 3 >= val,
                                             "<": 3 < val, "<=": 3 <= val}[OPS[op]]:
                            follow = True
                        continue
                    if fn in ("menumode", "getdisabled", "getsecondspassed", "random", "cellchanged",
                              "getcurrentaipackage", "getaipackagedone", "getsoundplaying", "gethealth"):
                        continue
                    if fn == "getjournalindex" and arg_str(a[0] if a else None):
                        m = meet(OPS.get(op, "="), val) if op else None
                        if m is not None and arg_str(a[0]).lower() != q:
                            steps.append(f"JOURNAL:{token(arg_str(a[0]))}:{int(m)}")
                        elif m is not None:
                            steps.append(f"JOURNAL:{token(q)}:{int(m)}")
                    elif fn == "ondeath" and owner and tags.get(owner) in ("NPC_", "CREA"):
                        action = ("KILL", owner)
                    elif fn == "onactivate" and owner:
                        action = ("ACTIVATE", owner)
                    elif fn in ("onpcadd",) and owner:
                        action = ("PICKUP", owner)
                    elif fn == "onpcequip" and owner:
                        steps.append(f"GIVE:{token(owner)}")
                        action = ("EQUIP", owner)
                    elif fn == "getitemcount" and arg_str(a[0] if a else None):
                        item = arg_str(a[0])
                        if (ref or "").lower() in ("player", "") and meet(OPS.get(op, ">="), val):
                            if owner and item.lower() == owner:
                                action = ("PICKUP", owner)
                            else:
                                steps.append(f"GIVE:{token(item)}:{int(max(1, meet(OPS.get(op, '>='), val)))}")
                    elif fn == "getdistance" and owner and op in ("<", "<="):
                        # a distance to something else (a shrine, a place): walk there; to the player: they're beside them
                        other = (arg_str(a[0]) or "").lower() if a else ""
                        action = action or ("WALKTO", other if other and other != "player" and other in where else owner)
                    elif fn == "getpccell" and arg_str(a[0] if a else None):
                        # GetPCCell "X" == 1: go there; == 0 (the escort has left it): go anywhere else
                        want_in = {"=": val == 1, "!=": val != 1}.get(OPS.get(op, "="), True) if op else True
                        action = action or (("GOTO", arg_str(a[0])) if want_in else ("AWAY", arg_str(a[0])))
                    elif fn == "getdeadcount" and arg_str(a[0] if a else None):
                        action = ("KILL", arg_str(a[0]).lower())
                    elif fn == "getpcsleep":
                        action = action or ("SLEEP", "24")
                    elif fn in ("gamehour", "getcurrenttime"):
                        steps.append(f"HOUR:{int(exact(OPS.get(op, '='), val)) % 24}")
                    elif fn in globals_ or (ref is None and fn not in ("journal",)):
                        if fn in globals_:
                            m = meet(OPS.get(op, "="), val) if op else 1
                            if m is not None:
                                steps.append(f"SETGLOBAL:{fn}:{m}")
                        else:
                            why = why or f"depends on {fn}"
            # where the player has to be: the owner's own cell for anything done at the owner; else say so
            if cellreq and owner and (action is None or action[0] in ("KILL", "ACTIVATE", "WALKTO", "PICKUP", "EQUIP")):
                own_cell = where[owner][1] if where[owner][0] == "int" else None
                for cname, must_in in cellreq:
                    if (own_cell is not None and own_cell.lower() == cname.lower()) != must_in:
                        why = why or (f"needs the player {'in' if must_in else 'out of'} {cname} while {owner} is "
                                      f"{'elsewhere' if must_in else 'there'}")
            # a global script counting days (strong_build1_h ...): started, then a week goes by away from the site
            days = name in started and re.search(r"(?i)\bday\b", text) and "dayspassed" in text.lower()
            if days:
                steps.append(f"STARTSCRIPT:{name}")
                if action is None:
                    action = ("AWAY", "Odai Plateau")
                action = action + ("DAYS",)
            # a kill the script sorts by whether the crime was seen (Morag Tong writs: 80 seen, 90 not): unseen
            if action and action[0] == "KILL" and re.search(r"(?i)getpccrimelevel", text):
                # Morag Tong writs: the script says 80 when the killing was seen, 90 when not; who sees it
                # depends on the cell, so the 90 branch belongs to a hand-written test (hw-writ)
                if re.search(r"(?i)journal\s+\w+\s+90", text) and v == 90:
                    why = why or "needs the kill unseen (witnesses decide 80 or 90: tests/hw-writ)"
            if action is None:
                why = why or ("no action found" if owner or name in started else "script never started / attached")
            target = action[1] if action else None
            place = where.get(target) if target and action[0] not in ("GOTO", "AWAY") else None
            if follow and owner and action and where.get(owner) is None:
                why = why or f"{owner} not placed in a cell"
            if action and action[0] not in ("GOTO", "AWAY") and place is None:
                why = why or f"{target} not placed in a cell"
            tests.append((q, v, name, steps + (["FOLLOW_OWNER:" + owner] if follow and owner else []), action, place, why))

    plan, files = {}, []
    runnable = [t for t in tests if not t[6]]
    for k in range(0, len(tests), args.batch):
        pass
    # batches of runnable tests; skipped ones listed in the plan too
    for k in range(0, max(1, len(runnable)), args.batch):
        name = f"xrun-{k // args.batch + 1}"
        lines = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 GOD", "0.3 0 0 0 0 BOOST:100", "0.3 0 0 0 0 GIVE:silver_longsword",
                 "0.3 0 0 0 0 EQUIP:silver_longsword", "1 0 0 0 0 SAVE:xrun"]
        entries = []
        for q, v, script, steps, action, place, why in runnable[k:k + args.batch]:
            lines += ["0.5 0 0 0 0 LOAD:xrun", "3 0 0 0", "0.3 0 0 0 0 LEVEL:30"]
            # a quest's journal set up once, to the highest value any condition asks (setting it lower after
            # would undo a guard's: MT_WritGuril 10, then 1)
            top = {}
            for st in steps:
                if st.startswith("JOURNAL:"):
                    q_, v_ = st[8:].rsplit(":", 1)
                    top[q_] = max(top.get(q_, -10**9), int(v_))
            setup = []
            for st in steps:
                if st.startswith("JOURNAL:"):
                    q_ = st[8:].rsplit(":", 1)[0]
                    if q_ in top:
                        setup.append(f"JOURNAL:{q_}:{top.pop(q_)}")
                else:
                    setup.append(st)
            for st in [x for x in setup if x.startswith("FOLLOW_OWNER:")]:
                # the escort under way: at their place, following the player before the action
                owner_ = st.split(":", 1)[1]
                op_ = where[owner_]
                if op_[0] == "int":
                    lines += [f"0.3 0 0 0 0 GOTO:{token(op_[1])}", "8 0 0 0"]
                else:
                    x, y, z = op_[1]
                    lines += [f"0.3 0 0 0 0 TP:{int(x)}:{int(y + 300)}:{int(z + 300)}:180", "8 0 0 0"]
                lines += [f"0.3 0 0 0 0 FOLLOW:{token(owner_)}", "4 0 0 0"]
            setup = [x for x in setup if not x.startswith("FOLLOW_OWNER:")]
            local_sets = [x for x in setup if x.startswith("SETLOCAL:")]      # once the owner is loaded
            setup = [x for x in setup if not x.startswith("SETLOCAL:")]
            lines += ["0.3 0 0 0 0 " + s for s in dict.fromkeys(setup)]
            verb, target = action[0], action[1]
            if verb == "AWAY":
                # anywhere outdoors but the named place: Seyda Neen's start spot (Balmora's when that is the place)
                spot = "-21160:-16810:700" if target.lower().startswith("seyda") else "-12104:-74572:400"
                lines += [f"0.3 0 0 0 0 TP:{spot}:90", "8 0 0 0"]
                if "DAYS" in action:
                    for _ in range(7):
                        lines += ["0.3 0 0 0 0 SLEEP:24:2", "3 0 0 0"]
                    lines += ["2 0 0 0"]
            elif verb == "GOTO":
                lines += [f"0.3 0 0 0 0 GOTO:{token(target)}", "8 0 0 0"]
            else:
                if place[0] == "int":
                    lines += [f"0.3 0 0 0 0 GOTO:{token(place[1])}", "8 0 0 0"]
                else:
                    x, y, z = place[1]
                    lines += [f"0.3 0 0 0 0 TP:{int(x)}:{int(y + 300)}:{int(z + 300)}:180", "8 0 0 0"]
                lines += ["0.3 0 0 0 0 " + x for x in dict.fromkeys(local_sets)]
                if verb == "SLEEP":
                    lines += ["0.3 0 0 0 0 SLEEP:24:2", "10 0 0 0 0 MSG"]
                elif verb == "EQUIP":
                    lines += [f"0.5 0 0 0 0 EQUIP:{token(target)}", "3 0 0 0 0 MSG"]
                elif "QUIET" in action:
                    lines += [f"0.3 0 0 0 0 SOULKILL:{token(target)}", "3 0 0 0 0 MSG"]
                else:
                    lines += [f"120 0 0 0 0 {verb}:{token(target)}", "3 0 0 0 0 MSG"]
            lines += ["2 0 0 0 0 MSG", f"0.3 0 0 0 0 CHECK:journal:{token(q)}", "0.3 0 0 0 0 B", "0.5 0 0 0 0 B"]
            entries.append([q, v, ""])
        (ROOT / "tools" / "tests" / f"{name}.txt").write_text("\n".join(lines) + "\n")
        plan[name] = entries
        files.append(name)
    plan["skipped"] = [[q, v, f"{script}: {why}"] for q, v, script, s, a, p, why in tests if why]
    (ROOT / "build" / "xrun.json").write_text(json.dumps(plan, indent=1))
    reasons = defaultdict(int)
    for q, v, script, s, a, p, why in tests:
        if why:
            reasons[why.split(" ")[0] + " " + " ".join(why.split(" ")[1:3])] += 1
    print(f"{len(tests)} script-set stages ({len(runnable)} to run in {len(files)} batches, {len(tests) - len(runnable)} skipped)")
    for r, n in sorted(reasons.items(), key=lambda kv: -kv[1])[:10]:
        print(f"   {n:4d}  {r}")


if __name__ == "__main__":
    main()
