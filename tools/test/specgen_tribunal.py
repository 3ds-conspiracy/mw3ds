"""Writes the Tribunal add-on's record cases from Tribunal.esm and the converted Tribunal data: one check for every
spell, ability, disease and power it adds (trib-spells), every enchantment it adds on an item (trib-enchantments),
every creature it adds (trib-creatures) and every armour and weapon it adds (trib-gear). All run on the Tribunal data.

  python3.13 tools/test/specgen_tribunal.py [--data out/tribunal]
  python3.13 tools/test/run_test.py trib-spells --native --data out/tribunal --start 'Seyda Neen'

Spells are checked by what they do: a lasting effect on the player by its magnitude (a fortify or drain of an
attribute or skill by the number itself), one on a target by its magnitude on Tribunal's test NPC Todd placed in front of the
player and held still, harm by the guard's health, a restore by the stat it fills. Enchantments the same way: a
constant one once the item is worn, a strike one by a blow (and the charge it spends), a used one by a cast. An
ability only actors have (the converter folds those into the actor) is checked on one of them where they stand. A
record nothing in the game data reaches (no item, no merchant, no script) gets a note instead of a check.
"""
import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "convert"))
os.environ["MW3DS_TRIBUNAL"] = "1"
import mwfiles  # noqa: E402
from build_game import CONSTANT_TARGET_EFFECTS  # noqa: E402

CASES = HERE / "cases"
TARGET = "todd"            # 304 health, not hostile, can be placed again and again (Tribunal's own test NPC)
HARM = {14, 15, 16, 18, 23, 27, 86}           # fire, shock, frost, drain / damage health, poison, absorb health
RESTORE = {75: "health", 76: "magicka", 77: "fatigue"}
FORTIFY_MAX = {80: "healthmax", 81: "magickamax", 82: "fatiguemax", 84: "magickamax"}
ATTRS = ["strength", "intelligence", "willpower", "agility", "speed", "endurance", "personality", "luck"]
SKILLS = ["block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade", "axe", "spear", "athletics",
          "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration", "alchemy",
          "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman", "mercantile",
          "speechcraft", "handtohand"]
SPELL_TYPES = {0: "spell", 1: "ability", 2: "blight", 3: "disease", 4: "curse", 5: "power"}
ENCH_TYPES = ["cast once", "on strike", "when used", "constant"]
AMMO = {11, 12, 13}                            # thrown weapons, arrows, bolts: no charge spent on a hit


def records(path, tags):
    out = {}
    for r in mwfiles.read_records(path, tags):
        name = r.zstr("NAME")
        if name:
            out[(r.tag, name.lower())] = r
    return out


def section(data, name, key):
    """The ids in a converted game section (one file or its parts), lowercased."""
    ids = set()
    for f in sorted(data.glob(f"game_{name}*.json")):
        b = f.read_bytes()
        j = json.loads(zlib.decompress(b[8:]) if b[:4] == b"MWZ1" else b)
        v = j.get(key, j)
        ids |= {k.lower() for k in (v.keys() if isinstance(v, dict) else [x["id"] for x in v])}
    return ids


def effects(r):
    out = []
    for t, d in r.subs:
        if t == "ENAM":
            eid, skill, attr, rng, area, dur, mn, mx = struct.unpack_from("<hbbiiiii", d)
            out.append({"id": eid, "skill": skill, "attr": attr, "range": rng, "area": area, "dur": dur, "min": mn})
    return out


def sub_int(r, tag, off=0, fmt="<i"):
    for t, d in r.subs:
        if t == tag:
            return struct.unpack_from(fmt, d, off)[0]
    return None


def token(id_):
    return id_.replace(" ", "%")


def cell_token(name):
    return name.replace(" ", "_")


class Case:
    def __init__(self, name, about):
        self.lines = ["# start: Seyda Neen", "# data: tribunal",
                      f"# {name}: written by tools/test/specgen_tribunal.py (edit the generator, not this file).",
                      f"# {about}",
                      "3 0 0 0 0 MSG",
                      "0.3 0 0 0 0 SEED:1+GOD+HOUR:12",
                      "0.3 0 0 0 0 HIDEACTORS:4000"]
        self.name = name

    def step(self, tok, secs=0.3):
        self.lines.append(f"{secs} 0 0 0 0 {tok}")

    def wait(self, secs):
        self.lines.append(f"{secs} 0 0 0")

    def note(self, text):
        self.lines.append(f"# {text}")

    def write(self):
        (CASES / f"{self.name}.txt").write_text("\n".join(self.lines) + "\n", encoding="utf-8")
        print(f"{self.name}: {sum(1 for ln in self.lines if 'EXPECT:' in ln)} checks")


def caster(c):
    # a caster no cast fails for: every school far over the hardest spell's cost
    for school in ("destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration"):
        c.step(f"SETSKILL:{school}:600")
    c.step("SETATTR:willpower:100+SETATTR:luck:100")
    c.step("RESISTROLL:99")          # (no resisting by the die: the target's willpower would turn paralysis away)
    c.step("GOD:2")                  # (kept alive, but blows hurt as usual: a god's blow kills the target first)
    # (an area spell at someone 180 units off catches the caster too, as in the game: the player shrugs it off)
    for resist in (90, 91, 92, 97):      # fire, frost, shock, poison
        c.step(f"EFFECT:{resist}:100:100000:0")


def stat_key(e):
    """The number a lasting effect on the player moves, or None to read it as the effect's total."""
    if e["id"] in (79, 17) and 0 <= e["attr"] < 8:
        return f"attr:{ATTRS[e['attr']]}"
    if e["id"] in (83, 21) and 0 <= e["skill"] < 27:
        return f"skill:{SKILLS[e['skill']]}"
    if e["id"] in FORTIFY_MAX:
        return FORTIFY_MAX[e["id"]]
    return None


def lasting_check(c, effs, give):
    """The effects are there once `give` (steps) has run and stay: abilities, diseases, worn items."""
    want = {}
    for e in effs:
        key = stat_key(e) or f"effect:{e['id']}"
        sign = -1 if e["id"] in (17, 21) else 1
        want[key] = want.get(key, 0) + sign * e["min"]
    for e in effs:
        if e["id"] in (17, 21) and stat_key(e):        # a drain needs room to drain: the number set well up first
            kind, name = stat_key(e).split(":")
            c.step(f"SET{'ATTR' if kind == 'attr' else 'SKILL'}:{name}:60")
    for key in want:
        c.step(f"SNAP:{key}")
    give()
    c.wait(0.5)
    for key, n in want.items():
        c.step(f"EXPECT:{key}:le:@{n}" if n < 0 else f"EXPECT:{key}:ge:@{n}")


def self_check(c, e, before):
    """Steps around a cast on the player for effect e (before: ahead of the cast); False: nothing to see."""
    eid = e["id"]
    if eid in RESTORE:
        if before:
            c.step("SETHEALTH:0.3" if eid == 75 else f"SET{RESTORE[eid].upper()}:5")
            c.step(f"SNAP:{RESTORE[eid]}")
        else:
            c.step(f"EXPECT:{RESTORE[eid]}:gt:@0")
        return True
    if eid == 74:      # restore attribute: damaged first
        attr = ATTRS[max(0, e["attr"])]
        if before:
            c.step(f"EFFECT:22:10:1:{max(0, e['attr'])}")
            c.wait(1.5)
            c.step(f"EXPECT:attrdamage:{attr}:gt:0")
        else:
            c.step(f"EXPECT:attrdamage:{attr}:lt:10")
        return True
    if eid == 69:      # cure common disease: caught first
        if before:
            c.step("SPELL:rotbone")
            c.step("EXPECT:diseases:ge:1")
        else:
            c.step("EXPECT:diseases:eq:0")
        return True
    if eid == 57 or e["dur"] <= 1:
        return False
    key = stat_key(e)
    if before:
        if key:
            c.step(f"SNAP:{key}")
    elif key:
        c.step(f"EXPECT:{key}:le:@-{e['min']}" if eid in (17, 21) else f"EXPECT:{key}:ge:@{e['min']}")
    else:
        c.step(f"EXPECT:effect:{eid}:ge:{e['min']}")
    return True


# Effects on someone's mind: read off their AI, not as an effect on them; humanoid ones (odd) or creature ones (even)
MIND = {49: ("calm", 0), 50: ("calm", 1), 51: ("frenzy", 0), 52: ("frenzy", 1), 53: ("flee", 0), 54: ("flee", 1),
        55: ("rally", 0), 56: ("rally", 1)}


def target_check(c, e, before):
    eid = e["id"]
    if eid in MIND:
        what, creature = MIND[eid]
        if creature:            # (the target is a person: a creature's calm or frenzy passes them by)
            return False
        if what == "calm" and before:
            c.step(f"HOSTILE:{TARGET}")
        elif what == "calm":
            c.step(f"EXPECT:refcombat:{TARGET}:eq:0")
        elif what == "frenzy" and not before:
            c.step(f"EXPECT:refcombat:{TARGET}:eq:1")
        elif what == "flee" and not before:
            c.step(f"EXPECT:refflee:{TARGET}:eq:1")
        return what != "rally"
    if eid in HARM and e["dur"] <= 1:      # (a lasting one: by its magnitude, a posed actor's effects don't tick)
        if before:
            c.step(f"SNAP:refhealth:{TARGET}")
        else:
            c.step(f"EXPECT:refhealth:{TARGET}:lt:@0")
        return True
    if eid == 57 or e["dur"] <= 1:
        return False
    if not before:
        c.step(f"EXPECT:refeffect:{TARGET},{eid}:gt:0")
    return True


def place_target(c, effs=()):
    c.step(f"PLACE:{TARGET}:180", 0.1)
    c.step(f"POSE:{TARGET}:idle:0:180")
    c.wait(0.5)


def done_target(c):
    c.step(f"SLAY:{TARGET}")
    c.wait(0.3)
    c.step(f"DISABLE:{TARGET}")
    c.step("SETBOUNTY:0")               # (the harm done to Todd is a crime: no guards coming)
    c.step("HIDEACTORS:4000")


def checked_cast(c, cast_tok, effs):
    """A cast with every effect it has a check for, and the cast counted."""
    on_self = [e for e in effs if e["range"] == 0]
    on_target = [e for e in effs if e["range"] != 0]
    if on_target:
        place_target(c, effs)
    c.step("SNAP:casts")
    for e in on_self:
        self_check(c, e, True)
    for e in on_target:
        target_check(c, e, True)
    c.step("FILL+SETMAGICKA:3000")
    c.step(f"CASTAT:{TARGET}:{cast_tok}" if on_target else f"CAST:{cast_tok}")
    c.wait(1.2)
    c.step("EXPECT:casts:gt:@0")
    for e in on_self:
        self_check(c, e, False)
    for e in on_target:
        target_check(c, e, False)
    if on_target:
        done_target(c)
    c.wait(0.5)


def where(T, actor_ids):
    """{actor id: interior cell name} for the first placed reference of each (the merged game's cells)."""
    out = {}
    for r in mwfiles.read_game({"CELL"}):
        flags = sub_int(r, "DATA")
        name = r.zstr("NAME") or ""
        if flags is None or not flags & 1 or not name:
            continue
        for ref in mwfiles.cell_refs(r):
            rid = (ref["id"] or "").lower()
            if rid in actor_ids and rid not in out:
                out[rid] = name
    return out


def visit(c, aid, cells):
    """Steps that bring the actor aid in front of the player: its own cell, else placed."""
    t = token(aid)
    if aid in cells:
        c.step(f"GOTO:{cell_token(cells[aid])}", 3.0)
    else:
        c.step(f"PLACE:{t}:300")
        c.wait(1.5)
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", default=str(HERE.parent.parent / "out" / "tribunal"),
                    help="converted Tribunal data: which records a player can reach")
    args = ap.parse_args()
    data = Path(args.data)
    known_spells = section(data, "spells", "spells")
    known_objects = section(data, "objects", "objects") | section(data, "actors", "actors")

    order = mwfiles.load_order()
    van, trib = order[0], order[1]
    tags = {"SPEL", "ENCH", "CREA", "NPC_", "ARMO", "WEAP", "CLOT", "BOOK"}
    V, T = records(van, tags), records(trib, tags)
    new = {k: r for k, r in T.items() if k not in V}
    carriers = {}
    for (tag, aid), r in T.items():
        if tag in ("NPC_", "CREA"):
            for t, d in r.subs:
                if t == "NPCS":
                    carriers.setdefault(mwfiles.text(d).lower(), []).append(aid)
    actors = {aid for (tag, aid) in new if tag == "CREA"} | {a for v in carriers.values() for a in v}
    cells = where(T, actors)

    # --- spells (the actors' own abilities last: going to them leaves Seyda Neen, where the guard can be placed)
    c = Case("trib-spells", "Every spell, ability, disease and power Tribunal adds, checked by what it does.")
    caster(c)
    c.step("SETUP-END", 0)
    later = []
    for (tag, sid), r in sorted(new.items()):
        if tag != "SPEL":
            continue
        kind = SPELL_TYPES.get(sub_int(r, "SPDT"), "?")
        effs = effects(r)
        c.note(f"{sid} ({kind}, {r.zstr('FNAM') or ''})")
        if sid in known_spells and kind in ("ability", "disease", "blight", "curse"):
            lasting_check(c, effs, lambda: c.step(f"SPELL:{token(sid)}"))
        elif sid in known_spells:
            c.step(f"SPELL:{token(sid)}")
            checked_cast(c, token(sid), effs)
        elif kind == "ability" and sid in carriers:
            kept = [e for e in effs if e["id"] in CONSTANT_TARGET_EFFECTS]
            who = sorted(carriers[sid], key=lambda a: (a not in cells, a))[0]
            if not kept:
                c.note(f"  only fortifies, which the converter folds into {who}'s numbers")
                continue
            c.note(f"  on {who}, who has it: checked at the end")
            later.append((sid, who, kept))
        elif sid in carriers:
            c.note(f"  only {', '.join(sorted(carriers[sid]))} know it; not a spell they cast in a fight (open)")
        else:
            c.note("  nothing in the game uses it")
    for sid, who, kept in later:
        c.note(f"{sid} on {who}")
        t = visit(c, who, cells)
        sums = {}
        for e in kept:
            sums[e["id"]] = sums.get(e["id"], 0) + e["min"]
        for eid, n in sums.items():
            c.step(f"EXPECT:refeffect:{t},{eid}:ge:{n}")
    c.write()

    # --- enchantments on items
    ench = {sid: r for (tag, sid), r in new.items() if tag == "ENCH"}
    items = {}
    for (tag, iid), r in sorted(T.items(), key=lambda kv: (kv[0][1] not in known_objects, kv[0])):
        e = r.zstr("ENAM") if tag in ("ARMO", "WEAP", "CLOT", "BOOK") else None
        if e and e.lower() in ench and e.lower() not in items:
            items[e.lower()] = (tag, iid, r)
    c = Case("trib-enchantments", "Every enchantment Tribunal adds, on an item that carries it, checked by what it does.")
    caster(c)
    c.step("SETUP-END", 0)
    for eid_, r in sorted(ench.items()):
        if eid_ not in items:
            c.note(f"{eid_}: no item carries it")
            continue
        tag, iid, rec = items[eid_]
        etype = sub_int(r, "ENDT")
        effs = effects(r)
        it = token(iid)
        c.note(f"{eid_} on {iid} ({ENCH_TYPES[etype]})")
        if iid not in known_objects:
            c.note("  nothing in the game data gives or carries the item")
            continue
        if etype == 3:
            lasting_check(c, effs, lambda: c.step(f"GIVE:{it}", 1.0))
            c.step(f"EXPECT:equipped:{it}:eq:1")
        elif etype == 1:
            wtype = sub_int(rec, "WPDT", 8, "<h") if tag == "WEAP" else None
            ammo = wtype in AMMO
            if wtype in (12, 13):        # arrows, bolts: shot from a bow / crossbow
                bow = "long%bow" if wtype == 12 else "steel%crossbow"
                c.step(f"GIVE:{bow}", 1.0)
                c.step(f"EQUIP:{bow}", 1.0)
            c.step(f"GIVE:{it}:5" if ammo else f"GIVE:{it}", 1.0)
            c.step(f"EQUIP:{it}", 1.0)
            place_target(c)
            if ammo:
                # thrown or shot as the player does (a hit spends no charge: the missile carries it)
                c.step("SETSKILL:marksman:100")
                c.step(f"FACE:{TARGET}")
                c.step("X", 1.2)
                c.wait(1.5)
            else:
                c.step(f"SNAP:charge:{it}")
                c.step(f"HIT:{TARGET}")
                c.wait(0.8)
                c.step(f"EXPECT:charge:{it}:lt:@0")
            on_target = [e for e in effs if e["range"] != 0]
            moods = {MIND[e["id"]][0] for e in on_target if e["id"] in MIND}
            if {"calm", "frenzy"} <= moods:       # (the Bipolar Blade: both at once, nothing certain to read)
                c.note("  calms and frenzies at once: its other effects checked")
                on_target = [e for e in on_target if e["id"] not in MIND]
            for e in on_target:
                if e["id"] in MIND:
                    target_check(c, e, False)
                elif e["dur"] > 1 and e["id"] != 57:
                    c.step(f"EXPECT:refeffect:{TARGET},{e['id']}:gt:0")
            done_target(c)
        elif etype == 2:
            c.step(f"GIVE:{it}", 1.0)
            c.step(f"SNAP:charge:{it}")
            checked_cast(c, f"item:{it}", effs)
            c.step(f"EXPECT:charge:{it}:lt:@0")
        else:
            c.step(f"GIVE:{it}", 1.0)
            checked_cast(c, f"item:{it}", effs)
            c.step(f"EXPECT:item:{it}:eq:0")
    c.write()

    # --- creatures
    c = Case("trib-creatures", "Every creature Tribunal adds: it is drawn, stands and animates, and dies.")
    c.step("SETUP-END", 0)
    for (tag, cid), r in sorted(new.items()):
        if tag != "CREA":
            continue
        c.note(f"{cid} ({r.zstr('FNAM') or ''}, {r.zstr('MODL') or ''})")
        if cid not in known_objects and cid not in cells:
            c.note("  nothing in the game data places it")
            continue
        t = visit(c, cid, cells)
        c.step(f"EXPECT:refmeshes:{t}:gt:0")
        if sub_int(r, "NPDT", 40) == 0:
            c.note("  no health: it lies dead where it is placed")
            c.step(f"EXPECT:dead:{t}")
            continue
        c.step(f"EXPECT:alive:{t}")
        c.step(f"EXPECT:refhealth:{t}:gt:0")
        c.step(f"SLAY:{t}")
        c.wait(2.5)
        c.step(f"EXPECT:dead:{t}")
    c.write()

    # --- gear
    c = Case("trib-gear", "Every armour and weapon Tribunal adds: carried, then worn or held.")
    c.step("SETUP-END", 0)
    for (tag, iid), r in sorted(new.items()):
        if tag not in ("ARMO", "WEAP"):
            continue
        it = token(iid)
        c.note(f"{iid} ({r.zstr('FNAM') or ''})")
        if iid not in known_objects:
            c.note("  nothing in the game data gives or carries it")
            continue
        c.step(f"GIVE:{it}", 1.0)
        c.step(f"EXPECT:item:{it}:ge:1")
        c.step(f"EXPECT:equipped:{it}:eq:1")
        if tag == "ARMO":
            c.step("EXPECT:armor:gt:0")
    c.write()


if __name__ == "__main__":
    main()
