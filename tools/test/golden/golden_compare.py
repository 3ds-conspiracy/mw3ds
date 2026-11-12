"""Golden run, the comparison: what OpenMW worked out beside what the converter wrote.

    python tools/test/golden/golden_compare.py actors [--data out/world] [--golden build/golden/openmw-actors.json]
                                                      [--examples 5] [--field attributes,skills,...] [--esm]
    python tools/test/golden/golden_compare.py spells [--log build/native-golden/sd/3ds/mw3ds/log.txt]
    python tools/test/golden/golden_compare.py effects | items | alchemy | signs [--data out/world]
    python tools/test/golden/golden_compare.py equipment | factions | gmst | magic | world [--data out/world]
    python tools/test/golden/golden_compare.py leveled | cspells | cells [--log build/native-golden/sd/3ds/mw3ds/log.txt]

effects: each magic effect's school, base cost and flags beside the data's (build_game.py hardcoded_flags).
items: weapons, armor, clothing, books, lights, tools, apparatus, misc items, potions, ingredients and containers: their
records' numbers beside the converted objects'.
alchemy: potions' and ingredients' effects. signs: the spells each birthsign gives.
leveled: creature leveled lists' records, and at each player level the creatures OpenMW's picks gave beside every one
the engine's pick can give (a native run with DUMPLEVELED). cspells: creatures' spells and diseases (DUMPSPELLS).
equipment: what NPCs wear and wield, OpenMW's auto-equip beside the converter's choice. factions: NPCs' faction,
rank and reputation. gmst: the numeric settings the engine reads (gmstf), OpenMW's beside the data's or the code's
default. world: factions, classes, races, journal quest stages, regions' weather. magic: spell and enchantment records (type, cost, charge, flags, effects). cells: every cell's objects and doors (where each leads), beside a native run's DUMPCELLS.
spells: the castable spells each NPC knows, OpenMW's beside the engine's (the record's plus its autocalc picks, from a
native run of tools/test/cases/scratch/golden-spells.txt: a step with DUMPSPELLS).

Reads OpenMW's answers (golden_openmw.py) and the converter's actors (game_actors_*.json in the data folder: the numbers
the engine runs on), and lists where they differ: NPCs with the Autocalc flag apart from those with stored stats (which
only prove the records were read right), then creatures. Exit status 1 when anything differs. Exact match: no tolerance.
OpenMW's numbers are read some frames after the actor exists, so a script (the vampires' clan bonuses, the cattle's AI)
may already have changed them: actors with a script are reported apart, and only the others are clean evidence.
"""
import argparse
import glob
import json
import sys
import zlib
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import golden_openmw  # noqa: E402
ATTRS = ["strength", "intelligence", "willpower", "agility", "speed", "endurance", "personality", "luck"]
SKILLS = ["block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade", "axe", "spear", "athletics",
          "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration", "alchemy",
          "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman", "mercantile",
          "speechcraft", "handtohand"]
# The numbers compared: lists name their entries in the report
LISTS = {"attributes": ATTRS, "skills": SKILLS}
SCALARS = ["level", "health", "magicka", "fatigue", "fight", "flee", "alarm", "hello", "gold", "soul"]


def load_converted(data):
    """The converter's actors by lowercase id, NPCs and creatures apart (MWZ1: 4-byte tag, 4-byte size, then zlib)."""
    rows = {"npc": {}, "creature": {}}
    for f in sorted(glob.glob(str(Path(data) / "game_actors_*.json"))):
        b = Path(f).read_bytes()
        if b[:4] != b"MWZ1":
            sys.exit(f"golden_compare: {f} is not packed data")
        for a in json.loads(zlib.decompress(b[8:]).decode("latin-1"))["actors"]:
            rows["creature" if a.get("creature") else "npc"][a["id"].lower()] = a
    return rows


def load_from_esm():
    """The converter's NPC numbers worked out now from the game's records (tools/convert/npcstats.py), without a
    conversion: the fast check while changing it. Creatures are not covered."""
    sys.path.insert(0, str(ROOT / "tools" / "convert"))
    import struct
    import mwfiles
    import npcstats
    db = mwfiles.load_db()
    rows = {"npc": {}, "creature": {}}
    for r in mwfiles.read_game():
        if r.tag != "NPC_":
            continue
        npdt = r.get("NPDT")
        rows["npc"][r.zstr("NAME").lower()] = dict(npcstats.npc_stats(r, db), level=struct.unpack_from("<h", npdt)[0],
                                                   script=r.zstr("SCRI") or "")
    return rows


def differences(want, got):
    """(field, detail) for each field of one actor that differs; want = OpenMW's, got = the converter's."""
    out = []
    for name in SCALARS:
        if name in want and name in got and want[name] != got[name]:
            out.append((name, f"openmw {want[name]}, converter {got.get(name)}"))
    for name, names in LISTS.items():
        if name not in want:
            continue
        bad = [f"{names[i]} {w} -> {g}" for i, (w, g) in enumerate(zip(want[name], got[name])) if w != g]
        if bad:
            out.append((name, f"{len(bad)} differ: " + ", ".join(bad[:4]) + (" ..." if len(bad) > 4 else "")))
    if "attack" in want and want["attack"] != [x for pair in got.get("attack", []) for x in pair]:
        out.append(("attack", f"openmw {want['attack']}, converter {got.get('attack')}"))
    return out


def compare(kind, golden, conv, label_of, args, only):
    """One report for a group of actors; the number of those that differ."""
    seen, anybad = Counter(), Counter()
    bad = defaultdict(Counter)
    examples = defaultdict(lambda: defaultdict(list))
    for rid, want in sorted(golden.items()):
        got = conv.get(rid)
        if not got:
            continue
        label = label_of(want) + (", with a script" if got.get("script") else ", no script")
        seen[label] += 1
        diffs = [d for d in differences(want, got) if not only or d[0] in only]
        anybad[label] += bool(diffs)
        for field, detail in diffs:
            bad[label][field] += 1
            if len(examples[label][field]) < args.examples:
                examples[label][field].append(f"{rid}: {detail}")
    missing = sum(1 for i in golden if i not in conv)
    print(f"\n{kind}: OpenMW made {len(golden)}, the data has {len(conv)}; {missing} of OpenMW's are not in the data "
          f"(not placed in the world))")
    for label in sorted(seen):
        print(f"  {label}: {seen[label]} compared, {anybad[label]} differ in something")
        for field, n in bad[label].most_common():
            print(f"    {field}: {n}")
            for e in examples[label][field]:
                print(f"        {e}")
    return sum(anybad.values())


def load_engine_spells(log):
    """{npc id: set of spell ids} from a native run's DUMPSPELLS lines (tools/test/cases/scratch/golden-spells.txt)."""
    rows = {}
    for ln in Path(log).read_text(errors="replace").splitlines():
        if "golden: npc\t" in ln:
            rows.setdefault(ln.split("golden: npc\t", 1)[1].strip(), set())
        elif "golden: spell\t" in ln:
            npc, spell = ln.split("golden: spell\t", 1)[1].strip().split("\t")
            rows.setdefault(npc, set()).add(spell.lower())
    if not rows:
        sys.exit(f"golden_compare: no DUMPSPELLS lines in {log}")
    return rows


def compare_spells(golden, engine, conv, args):
    """The castable spells (type 0) each NPC knows: OpenMW's beside the engine's. Abilities and powers (the race's) are
    left out: the engine keeps those apart. Returns the number of NPCs that differ."""
    kind = {sid: t for lst in golden["spells"].values() for sid, t in lst}
    seen, bad = Counter(), Counter()
    missing, extra, examples = Counter(), Counter(), defaultdict(list)
    for rid, want in sorted(golden["npc"].items()):
        if rid not in engine:
            continue
        label = ("autocalc" if want["autocalc"] else "stored stats") + (
            ", with a script" if (conv.get(rid) or {}).get("script") else ", no script")
        seen[label] += 1
        theirs = {sid for sid, t in golden["spells"].get(rid, []) if t == 0}
        ours = {sid for sid in engine[rid] if kind.get(sid, 0) == 0}
        if theirs != ours:
            bad[label] += 1
            missing.update(theirs - ours)
            extra.update(ours - theirs)
            if len(examples[label]) < args.examples:
                examples[label].append(f"{rid}: OpenMW only {sorted(theirs - ours)}, ours only {sorted(ours - theirs)}")
    print("\nSpells NPCs know (castable spells only)")
    for label in sorted(seen):
        print(f"  {label}: {seen[label]} compared, {bad[label]} differ")
        for e in examples[label]:
            print(f"        {e}")
    print(f"  most often OpenMW's only: {missing.most_common(8)}")
    print(f"  most often ours only: {extra.most_common(8)}")
    return sum(bad.values())


def load_packed(path):
    b = Path(path).read_bytes()
    return json.loads((zlib.decompress(b[8:]) if b[:4] == b"MWZ1" else b).decode("latin-1"))


# Magic effects: OpenMW's fields beside the converter's flag bits (build_game.py magic_effects / hardcoded_flags, in
# OpenMW's layout: components/esm3/loadmgef.hpp); a None bit is one the converter doesn't keep (only the school's
# skill and the cost are then compared)
EFFECT_BITS = {"duration": (0x4, True), "magnitude": (0x8, True), "harmful": (0x10, False),
               "continuousvfx": (0x20, False), "self": (0x40, False), "touch": (0x80, False),
               "target": (0x100, False), "spellmaking": (0x200, False), "enchanting": (0x400, False),
               "negativelight": (0x800, False), "appliedonce": (0x1000, False)}
SCHOOL_SKILL = ["alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration"]


def compare_effects(golden, data, args):
    """Each magic effect's school, base cost and flags: OpenMW's beside the converted data's. Returns the number that
    differ."""
    sys.path.insert(0, str(ROOT / "tools" / "convert"))
    from build_game import EFFECT_KEYS
    conv = load_packed(Path(data) / "game.json")["magic_effects"]
    by_name = {k.lower(): i for i, k in enumerate(EFFECT_KEYS)}
    bad, fields, unmatched = 0, Counter(), []
    for eid, want in sorted(golden["effects"].items()):
        # (the Lua table's keys count from 1: effect 0, Water Breathing, is key 1)
        idx = int(want["key"]) - 1 if str(want["key"]).isdigit() else by_name.get(eid)
        got = conv.get(str(idx)) if idx is not None else None
        if got is None:
            unmatched.append(eid)
            continue
        diffs = []
        if want["school"] != SCHOOL_SKILL[got["school"]] if 0 <= got["school"] < 6 else True:
            diffs.append(f"school {want['school']} / ours {got['school']}")
        if abs(want["cost"] - got["cost"]) > 1e-4:
            diffs.append(f"cost {want['cost']} / ours {got['cost']}")
        for name, (b, inverted) in EFFECT_BITS.items():
            ours = bool(got["flags"] & b) != inverted
            if bool(want[name]) != ours:
                diffs.append(f"{name} {want[name]} / ours {int(ours)}")
                fields[name] += 1
        if diffs:
            bad += 1
            print(f"  {idx} {eid}: " + ", ".join(diffs))
    print(f"\nMagic effects: {len(golden['effects'])} in OpenMW, {bad} differ; by field {dict(fields)}"
          + (f"; not matched to ours: {unmatched}" if unmatched else ""))
    return bad


# Items: OpenMW's record field -> the converter's key, per kind (game_objects_*.json); flags: (key, bit)
ITEM_FIELDS = {
    "Weapon": {"value": "value", "weight": "weight", "health": "health", "speed": "speed", "reach": "reach",
               "enchantCapacity": "enchant", "type": "wtype", "isMagical": ("flags", 1), "isSilver": ("flags", 2),
               "chopMinDamage": ("chop", 0), "chopMaxDamage": ("chop", 1), "slashMinDamage": ("slash", 0),
               "slashMaxDamage": ("slash", 1), "thrustMinDamage": ("thrust", 0), "thrustMaxDamage": ("thrust", 1)},
    "Armor": {"value": "value", "weight": "weight", "health": "health", "baseArmor": "armor",
              "enchantCapacity": "enchant", "type": "atype"},
    "Clothing": {"value": "value", "weight": "weight", "enchantCapacity": "enchant", "type": "ctype"},
    "Book": {"value": "value", "weight": "weight", "enchantCapacity": "enchant", "isScroll": "scroll"},
    "Light": {"value": "value", "weight": "weight", "duration": "time", "radius": "radius"},
    "Lockpick": {"value": "value", "weight": "weight", "quality": "quality", "maxCondition": "uses"},
    "Probe": {"value": "value", "weight": "weight", "quality": "quality", "maxCondition": "uses"},
    "Repair": {"value": "value", "weight": "weight", "quality": "quality", "maxCondition": "uses"},
    "Apparatus": {"value": "value", "weight": "weight", "quality": "quality", "type": "subtype"},
    "Miscellaneous": {"value": "value", "weight": "weight"},
    "Potion": {"value": "value", "weight": "weight"},
    "Ingredient": {"value": "value", "weight": "weight"},
    "Container": {"weight": "capacity", "isOrganic": "organic", "isRespawning": "respawn"},
}


def compare_items(golden, data, args):
    """Item records' numbers: OpenMW's beside the converted objects'. Returns the number of items that differ."""
    conv = {}
    for f in sorted(glob.glob(str(Path(data) / "game_objects_*.json"))):
        for k, v in load_packed(f)["objects"].items():
            conv[k.lower()] = v
    seen, bad, fields = Counter(), Counter(), Counter()
    examples = defaultdict(list)
    missing = Counter()
    for iid, want in sorted(golden["items"].items()):
        kind = want["kind"]
        got = conv.get(iid)
        if got is None:
            missing[kind] += 1
            continue
        seen[kind] += 1
        diffs = []
        for field, key in ITEM_FIELDS[kind].items():
            if field not in want or want[field] == "nil":
                continue
            w = float(want[field])
            # (units: OpenMW's Lua gives enchant capacity x fEnchantmentMult (0.1), the data keeps the record's own, as
            # the engine multiplies it in enchanting.cpp; a light that burns for ever: -1 in the record, 0 in the data)
            if field == "enchantCapacity":
                w *= 10.0
            if kind == "Light" and field == "duration" and w < 0:
                w = 0.0
            if isinstance(key, tuple):
                k, i = key
                if k == "flags":
                    g = 1.0 if got.get("flags", 0) & i else 0.0
                else:
                    g = float(got.get(k, [None, None])[i]) if got.get(k) else None
            else:
                g = got.get(key)
                g = float(g) if g is not None else None
            if g is None or abs(w - g) > 1e-3 * max(1.0, abs(w)):
                diffs.append(f"{field} {want[field]} / ours {g}")
                fields[(kind, field)] += 1
        if diffs:
            bad[kind] += 1
            if len(examples[kind]) < args.examples:
                examples[kind].append(f"{iid}: " + ", ".join(diffs[:5]))
    print("\nItems (OpenMW's records beside the converted objects)")
    for kind in sorted(seen):
        print(f"  {kind}: {seen[kind]} compared, {bad[kind]} differ" + (f", {missing[kind]} not in the data" if missing[kind] else ""))
        for e in examples[kind]:
            print(f"        {e}")
    if fields:
        print("  by field:", ", ".join(f"{k}.{f} {n}" for (k, f), n in fields.most_common()))
    return sum(bad.values())


def engine_lines(log, tag):
    """The tab-separated fields of a native run's "golden: <tag>" lines."""
    out = []
    for ln in Path(log).read_text(errors="replace").splitlines():
        if f"golden: {tag}\t" in ln:
            out.append(ln.split(f"golden: {tag}\t", 1)[1].rstrip("\r\n").split("\t"))
    return out


def effect_index(golden):
    """OpenMW's effect id ("firedamage") -> its number (the Lua table counts from 1)."""
    return {eid: int(e["key"]) - 1 for eid, e in golden["effects"].items()}


def compare_leveled(golden, data, log, args):
    """Creature leveled lists: the records beside the data's, and at each player level the creatures OpenMW's own picks
    gave (sampled) beside every creature the engine's pick can give. One OpenMW gave that ours can't is a difference;
    one ours can give that OpenMW's samples never did may be chance (a rare entry), so it is only counted."""
    conv = {k.lower(): v for k, v in load_packed(Path(data) / "game.json")["leveled"].items()}
    rec_bad = []
    for lid, want in sorted(golden["levrec"].items()):
        got = conv.get(lid)
        if got is None:
            rec_bad.append(f"{lid}: not in the data")
            continue
        theirs = sorted((int(lv), i.lower()) for i, lv in want["entries"])
        ours = sorted((int(lv), i.lower()) for lv, i in got.get("e", []))
        diffs = []
        if theirs != ours:
            diffs.append(f"entries OpenMW only {sorted(set(theirs) - set(ours))[:4]}, ours only {sorted(set(ours) - set(theirs))[:4]}")
        if round(want["none"] * 100) != int(got.get("none", 0)):
            diffs.append(f"chance none {want['none']} / ours {got.get('none', 0)}")
        if bool(want["all"]) != bool(got.get("all", 0)):
            diffs.append(f"all levels {want['all']} / ours {got.get('all', 0)}")
        if diffs:
            rec_bad.append(f"{lid}: " + "; ".join(diffs))
    eng = defaultdict(lambda: defaultdict(set))
    for lid, level, pick in engine_lines(log, "levpick"):
        eng[lid][level].add("" if pick == "-" else pick.lower())
    strong, weak, compared = [], 0, 0
    for lid, by_level in sorted(golden["levpick"].items()):
        for level, theirs in by_level.items():
            if lid not in eng:
                continue
            compared += 1
            ours = eng[lid][level]
            only_theirs = set(theirs) - ours
            if only_theirs:
                strong.append(f"{lid} at level {level}: OpenMW gave {sorted(only_theirs)}, ours can give {sorted(ours)}")
            weak += bool(ours - set(theirs))
    print(f"\nCreature leveled lists: {len(golden['levrec'])} records, {len(rec_bad)} differ from the data")
    for e in rec_bad[:args.examples]:
        print(f"        {e}")
    print(f"  picks: {compared} list-levels compared; OpenMW gave a creature ours can't in {len(strong)}; ours can give "
          f"one OpenMW's 150 samples didn't in {weak}")
    for e in strong[:args.examples]:
        print(f"        {e}")
    return len(rec_bad) + len(strong)


def compare_creature_spells(golden, log, args):
    """Creatures' castable spells and diseases: OpenMW's beside the engine's (what it knows or casts; what it carries).
    Abilities are left out (the engine folds them into constant effects)."""
    eng = defaultdict(lambda: {"knows": set(), "disease": set()})
    for (cid,) in [ln[:1] for ln in engine_lines(log, "creature")]:
        eng[cid]
    # (castable spells only: a creature's list holds its abilities and diseases too, which OpenMW lists as such)
    kinds = {k.lower(): v.get("type", 0) for k, v in load_packed(Path(args.data) / "game_spells.json")["spells"].items()}
    for cid, sp in engine_lines(log, "cspell") + engine_lines(log, "ccast"):
        if kinds.get(sp, 0) == 0:
            eng[cid]["knows"].add(sp)
    for cid, sp in engine_lines(log, "cdisease"):
        eng[cid]["disease"].add(sp)
    bad, compared, examples = Counter(), 0, []
    for cid in sorted(golden["creature"]):
        if cid not in eng:
            continue
        compared += 1
        spells = golden["cspells"].get(cid, [])
        theirs = {s for s, t in spells if t == 0}
        dis = {s for s, t in spells if t in (2, 3)}
        diffs = []
        if theirs != eng[cid]["knows"]:
            diffs.append(f"spells OpenMW only {sorted(theirs - eng[cid]['knows'])}, ours only {sorted(eng[cid]['knows'] - theirs)}")
            bad["spells"] += 1
        if dis != eng[cid]["disease"]:
            diffs.append(f"diseases OpenMW only {sorted(dis - eng[cid]['disease'])}, ours only {sorted(eng[cid]['disease'] - dis)}")
            bad["diseases"] += 1
        if diffs and len(examples) < args.examples:
            examples.append(f"{cid}: " + "; ".join(diffs))
    print(f"\nCreature spells: {compared} creatures compared; differ in spells {bad['spells']}, in diseases {bad['diseases']}")
    for e in examples:
        print(f"        {e}")
    return sum(bad.values())


def compare_alchemy(golden, data, args):
    """Potions' and ingredients' effects: OpenMW's records beside the converted objects'. A skill / attribute counts only
    on an effect that has one."""
    idx = effect_index(golden)
    has = {idx[e]: (g["skill"], g["attribute"]) for e, g in golden["effects"].items()}
    conv = {}
    for f in sorted(glob.glob(str(Path(data) / "game_objects_*.json"))):
        for k, v in load_packed(f)["objects"].items():
            conv[k.lower()] = v
    seen, bad, examples, missing = Counter(), Counter(), [], Counter()
    for iid, want in sorted(golden["itemfx"].items()):
        kind, got = want["kind"], conv.get(iid)
        if got is None:
            missing[kind] += 1
            continue
        seen[kind] += 1
        theirs = []
        for eid, sk, at, mn, mx, dur, rng, area in want["effects"]:
            e = idx.get(eid.lower(), -1)
            s = SKILLS.index(sk.lower()) if sk and has.get(e, (0, 0))[0] else -1
            a = ATTRS.index(at.lower()) if at and has.get(e, (0, 0))[1] else -1
            theirs.append((e, s, a) if kind == "Ingredient" else (e, s, a, int(mn), int(mx), int(dur), int(rng), int(area)))
        if kind == "Ingredient":
            ours = [(e, s if has.get(e, (0, 0))[0] else -1, a if has.get(e, (0, 0))[1] else -1) for e, s, a in got.get("ingr", [])]
        else:
            ours = [(x["effect"], x["skill"] if has.get(x["effect"], (0, 0))[0] else -1,
                     x["attribute"] if has.get(x["effect"], (0, 0))[1] else -1, x["min"], x["max"], x["duration"],
                     x["range"], x.get("area", 0)) for x in got.get("effects", [])]
        if theirs != ours:
            bad[kind] += 1
            if len(examples) < args.examples:
                examples.append(f"{iid}: OpenMW {theirs}, ours {ours}")
    print("\nPotion and ingredient effects")
    for kind in sorted(seen):
        print(f"  {kind}: {seen[kind]} compared, {bad[kind]} differ" + (f", {missing[kind]} not in the data" if missing[kind] else ""))
    for e in examples:
        print(f"        {e}")
    return sum(bad.values())


def compare_signs(golden, data, args):
    """Birthsigns: the spells each gives, OpenMW's beside the data's."""
    conv = {b["id"].lower(): sorted(s.lower() for s in b["spells"])
            for b in load_packed(Path(data) / "game_birthsigns.json")["birthsigns"]}
    bad = [f"{k}: OpenMW {v}, ours {conv.get(k)}" for k, v in sorted(golden["signs"].items()) if conv.get(k) != v]
    print(f"\nBirthsigns: {len(golden['signs'])} compared, {len(bad)} differ")
    for e in bad:
        print(f"        {e}")
    return len(bad)


def compare_magic_records(golden, data, args):
    """Spell and enchantment records: type, cost, charge, flags and effects, OpenMW's beside the converted data's
    (game_spells.json; game.json enchantments). Only those in the data are compared (the converter keeps the ones the
    world uses). A skill / attribute counts only on an effect that has one."""
    idx = effect_index(golden)
    has = {idx[e]: (g["skill"], g["attribute"]) for e, g in golden["effects"].items()}

    def theirs_effects(effects):
        out = []
        for eid, sk, at, mn, mx, dur, rng, area in effects:
            e = idx.get(eid.lower(), -1)
            s = SKILLS.index(sk.lower()) if sk and has.get(e, (0, 0))[0] else -1
            a = ATTRS.index(at.lower()) if at and has.get(e, (0, 0))[1] else -1
            out.append((e, s, a, int(mn), int(mx), int(dur), int(rng), int(area)))
        return out

    def ours_effects(effects):
        return [(x["effect"], x["skill"] if has.get(x["effect"], (0, 0))[0] else -1,
                 x["attribute"] if has.get(x["effect"], (0, 0))[1] else -1, x["min"], x["max"], x["duration"],
                 x["range"], x.get("area", 0)) for x in effects]

    spells = load_packed(Path(data) / "game_spells.json")["spells"]
    spells = {k.lower(): v for k, v in spells.items()}
    ench = {k.lower(): v for k, v in load_packed(Path(data) / "game.json")["enchantments"].items()}
    total = 0
    for name, want_all, conv in (("Spells", golden.get("spellrec", {}), spells),
                                 ("Enchantments", golden.get("enchrec", {}), ench)):
        seen, bad, fields, examples = 0, 0, Counter(), []
        for rid, want in sorted(want_all.items()):
            got = conv.get(rid)
            if got is None:
                continue
            seen += 1
            diffs = []
            head = want["head"]
            if int(head[0]) != got.get("type"):
                diffs.append(f"type {int(head[0])} / ours {got.get('type')}")
            if int(head[1]) != got.get("cost"):
                diffs.append(f"cost {int(head[1])} / ours {got.get('cost')}")
            if name == "Spells":
                flags = got.get("flags", 0)
                for i, (label, b) in enumerate((("autocalc", 1), ("pc start", 2), ("always succeeds", 4))):
                    if bool(head[2 + i]) != bool(flags & b):
                        diffs.append(f"{label} {int(head[2 + i])} / ours {int(bool(flags & b))}")
            elif int(head[2]) != got.get("charge"):
                diffs.append(f"charge {int(head[2])} / ours {got.get('charge')}")
            if theirs_effects(want["effects"]) != ours_effects(got.get("effects", [])):
                diffs.append(f"effects OpenMW {theirs_effects(want['effects'])}, ours {ours_effects(got.get('effects', []))}")
            for d in diffs:
                fields[d.split(" ")[0]] += 1
            if diffs:
                bad += 1
                if len(examples) < args.examples:
                    examples.append(f"{rid}: " + "; ".join(diffs))
        print(f"\n{name}: {seen} in the data compared ({len(want_all)} in OpenMW), {bad} differ; by field {dict(fields)}")
        for e in examples:
            print(f"        {e}")
        total += bad
    return total


SPECIALIZATIONS = ["combat", "magic", "stealth"]
# A region's weather chances in the data, in the record's order (WEAT); OpenMW keys them by weather id
WEATHERS = ["clear", "cloudy", "foggy", "overcast", "rain", "thunderstorm", "ashstorm", "blight", "snow", "blizzard"]


def compare_world_records(golden, data, args):
    """Factions (favoured attributes / skills, reactions, rank requirements), classes, races, journal quest stages and
    regions' weather: OpenMW's records beside the converted data's. Returns the number that differ."""
    def fields(row, n):
        return (row + [""] * n)[:n]

    def sk(names):
        return [SKILLS.index(x.lower()) for x in names.split(",") if x]

    def at(names):
        return [ATTRS.index(x.lower()) for x in names.split(",") if x]

    report, total = [], 0

    def check(kind, seen, bad):
        nonlocal total
        total += len(bad)
        report.append(f"  {kind}: {seen} compared, {len(bad)} differ")
        report.extend(f"        {b}" for b in bad[:args.examples])

    # factions
    conv = {k.lower(): v for k, v in load_packed(Path(data) / "game_factions.json")["factions"].items()}
    bad, seen = [], 0
    for fid, row in sorted(golden.get("factionrec", {}).items()):
        got = conv.get(fid)
        if got is None:
            continue
        seen += 1
        attrs, skills, reactions, ranks = fields(row, 4)
        d = []
        if at(attrs) != list(got.get("attrs", [])):
            d.append(f"attributes {at(attrs)} / ours {got.get('attrs')}")
        if sorted(sk(skills)) != sorted(got.get("skills", [])):
            d.append(f"skills {sk(skills)} / ours {got.get('skills')}")
        theirs_r = {k.lower(): int(float(v)) for k, v in (p.rsplit(":", 1) for p in reactions.split(",") if p)}
        ours_r = {k.lower(): v for k, v in got.get("reactions", {}).items()}
        if theirs_r != ours_r:
            d.append(f"reactions differ: OpenMW only {sorted(set(theirs_r.items()) - set(ours_r.items()))[:3]}, "
                     f"ours only {sorted(set(ours_r.items()) - set(theirs_r.items()))[:3]}")
        theirs_k = [[int(float(x)) for x in r.split(":")] for r in ranks.split(",") if r]
        # (the data pads every faction to ten ranks with zeros; OpenMW lists the named ones)
        ours_k = [list(r) for r in got.get("reqs", [])]
        if theirs_k != ours_k[:len(theirs_k)] or any(any(r) for r in ours_k[len(theirs_k):]):
            d.append(f"rank requirements {theirs_k[:3]}... / ours {got.get('reqs', [])[:3]}...")
        if d:
            bad.append(f"{fid}: " + "; ".join(d))
    check("Factions", seen, bad)

    # classes
    conv = {c["id"].lower(): c for c in load_packed(Path(data) / "game_classes.json")["classes"]}
    bad, seen = [], 0
    for cid, row in sorted(golden.get("classrec", {}).items()):
        got = conv.get(cid)
        if got is None:
            continue
        seen += 1
        spec, attrs, major, minor, playable = fields(row, 5)
        d = []
        if SPECIALIZATIONS.index(spec.lower()) != got.get("specialization"):
            d.append(f"specialization {spec} / ours {got.get('specialization')}")
        if sorted(at(attrs)) != sorted(got.get("attributes", [])):
            d.append(f"attributes {at(attrs)} / ours {got.get('attributes')}")
        if sorted(sk(major)) != sorted(got.get("major", [])):
            d.append(f"major {sk(major)} / ours {got.get('major')}")
        if sorted(sk(minor)) != sorted(got.get("minor", [])):
            d.append(f"minor {sk(minor)} / ours {got.get('minor')}")
        if int(playable or 0) != int(got.get("playable", 0)):
            d.append(f"playable {playable} / ours {got.get('playable')}")
        if d:
            bad.append(f"{cid}: " + "; ".join(d))
    check("Classes", seen, bad)

    # races
    conv = {r["id"].lower(): r for r in load_packed(Path(data) / "game_races.json")["races"]}
    bad, seen = [], 0
    for rid, row in sorted(golden.get("racerec", {}).items()):
        got = conv.get(rid)
        if got is None:
            continue
        seen += 1
        playable, beast, skills, attrs, height, weight, spells = fields(row, 7)
        d = []
        theirs_s = sorted((SKILLS.index(k.lower()), int(float(v))) for k, v in (p.split(":") for p in skills.split(",") if p))
        if theirs_s != sorted(tuple(x) for x in got.get("skill_bonus", [])):
            d.append(f"skill bonuses {theirs_s} / ours {got.get('skill_bonus')}")
        theirs_a = {ATTRS.index(k.lower()): [int(float(m)), int(float(f))] for k, m, f in (p.split(":") for p in attrs.split(",") if p)}
        if [theirs_a.get(i) for i in range(8)] != [list(x) for x in got.get("attributes", [])]:
            d.append(f"attributes {[theirs_a.get(i) for i in range(8)]} / ours {got.get('attributes')}")
        for label, val, key in (("height", height, "height"), ("weight", weight, "weight")):
            t = [float(x) for x in val.split(":")]
            if any(abs(a - b) > 1e-4 for a, b in zip(t, got.get(key, []))):
                d.append(f"{label} {t} / ours {got.get(key)}")
        if int(playable or 0) != int(got.get("playable", 0)) or int(beast or 0) != int(got.get("beast", 0)):
            d.append(f"playable / beast {playable} {beast} / ours {got.get('playable')} {got.get('beast')}")
        if sorted(s.lower() for s in spells.split(",") if s) != sorted(s.lower() for s in got.get("spells", [])):
            d.append(f"spells {spells} / ours {got.get('spells')}")
        if d:
            bad.append(f"{rid}: " + "; ".join(d))
    check("Races", seen, bad)

    # journal quest stages
    conv = {}
    for f in sorted(glob.glob(str(Path(data) / "game_journal_*.json"))):
        for k, v in load_packed(f)["journal"].items():
            conv[k.lower()] = v
    bad, seen = [], 0
    for qid, stages in sorted(golden.get("jinfo", {}).items()):
        got = conv.get(qid)
        if got is None:
            continue
        seen += 1
        # (stages only: Morrowind.esm has no finished / restart flags; the converter takes them from Tribunal.esm and
        # Bloodmoon.esm, which add them for its quests: build_game.py plugin_journal_infos)
        theirs = sorted(t[0] for t in stages)
        ours = sorted(e["index"] for e in got.get("entries", []))
        if theirs != ours:
            bad.append(f"{qid}: stages OpenMW only {sorted(set(theirs) - set(ours))[:4]}, "
                       f"ours only {sorted(set(ours) - set(theirs))[:4]}")
    check("Journal quests", seen, bad)

    # regions
    conv = {k.lower(): v for k, v in load_packed(Path(data) / "game.json")["regions"].items()}
    bad, seen = [], 0
    for rid, row in sorted(golden.get("regionrec", {}).items()):
        got = conv.get(rid)
        if got is None:
            continue
        seen += 1
        weather, sleep = fields(row, 2)
        theirs = {k.lower(): int(float(v)) for k, v in (p.split(":") for p in weather.split(",") if p)}
        ours = {WEATHERS[i]: v for i, v in enumerate(got.get("chances", []))}
        d = []
        if {k: v for k, v in theirs.items() if v} != {k: v for k, v in ours.items() if v}:
            d.append(f"weather OpenMW {theirs} / ours {ours}")
        if sleep.lower() != (got.get("sleep") or "").lower():
            d.append(f"sleep list {sleep} / ours {got.get('sleep')}")
        if d:
            bad.append(f"{rid}: " + "; ".join(d))
    check("Regions", seen, bad)
    print("\nWorld records (OpenMW beside the converted data)")
    print("\n".join(report))
    return total


# OpenMW's EQUIPMENT_SLOT numbers: 16 the right hand (the weapon), 17 the left (a shield or a torch), 18 ammunition;
# the rest are worn
SLOT_WEAPON, SLOT_LEFT, SLOT_AMMO = "16", "17", "18"


def npcs_with_leveled_items():
    """The NPCs whose inventory holds a leveled item list: what they wear or wield is rolled, by OpenMW at random and
    by the converter once, so a difference there may be the dice."""
    sys.path.insert(0, str(ROOT / "tools" / "convert"))
    import mwfiles
    out = set()
    levi = set()
    recs = []
    for r in mwfiles.read_game():
        if r.tag == "LEVI":
            levi.add(r.zstr("NAME").lower())
        elif r.tag == "NPC_":
            recs.append(r)
    for r in recs:
        if any(mwfiles.text(d[4:]).lower() in levi for t, d in r.subs if t == "NPCO"):
            out.add(r.zstr("NAME").lower())
    return out


def compare_equipment(golden, conv, args):
    """What NPCs have on: OpenMW's (inventory filled, auto-equipped) beside the converter's baked choice (worn,
    weapon, shield, ammo). NPCs with a leveled item list are reported apart."""
    rolled = npcs_with_leveled_items()
    shields = set()
    for f in sorted(glob.glob(str(Path(args.data) / "game_objects_*.json"))):
        for k, v in load_packed(f)["objects"].items():
            if v.get("type") == "ARMO" and v.get("atype") == 8:
                shields.add(k.lower())
    seen, bad = Counter(), defaultdict(Counter)
    examples = defaultdict(list)
    for rid in sorted(golden["npc"]):
        got = conv.get(rid)
        if got is None:
            continue
        eq = golden.get("equip", {}).get(rid, {})
        label = "a leveled item list (rolled)" if rid in rolled else "no leveled list"
        seen[label] += 1
        # (shields apart: an idle NPC in OpenMW has none on; the converter bakes the one it takes up onto the body)
        theirs_worn = {i for s, i in eq.items() if s not in (SLOT_WEAPON, SLOT_LEFT, SLOT_AMMO)}
        ours_worn = {i.lower() for i in got.get("worn", []) if i.lower() not in shields}
        diffs = []
        if theirs_worn != ours_worn:
            diffs.append(("worn", f"OpenMW only {sorted(theirs_worn - ours_worn)}, ours only {sorted(ours_worn - theirs_worn)}"))
        for field, slot in (("weapon", SLOT_WEAPON), ("shield", SLOT_LEFT), ("ammo", SLOT_AMMO)):
            theirs, ours = eq.get(slot, ""), (got.get(field) or "").lower()
            # (a torch in the left hand is a light, not a shield: the converter keeps lights apart)
            if field == "shield" and theirs and not ours and "torch" in theirs:
                continue
            if theirs != ours:
                diffs.append((field, f"{field} OpenMW {theirs or '-'}, ours {ours or '-'}"))
        for field, d in diffs:
            bad[label][field] += 1
            if len(examples[(label, field)]) < args.examples:
                examples[(label, field)].append(f"{rid}: {d}")
    print("\nNPC equipment (OpenMW auto-equipped beside the converter's choice)")
    for label in sorted(seen):
        print(f"  {label}: {seen[label]} NPCs; differ in " + (", ".join(f"{f} {n}" for f, n in bad[label].most_common()) or "nothing"))
        for field, _ in bad[label].most_common():
            for e in examples[(label, field)]:
                print(f"        {e}")
    return sum(bad["no leveled list"].values())


def compare_factions(golden, conv, args):
    """NPCs' faction, rank and reputation: OpenMW's beside the converter's. OpenMW's Lua counts ranks from 1."""
    bad, examples, seen = Counter(), defaultdict(list), 0
    for rid in sorted(golden["npc"]):
        got = conv.get(rid)
        if got is None:
            continue
        seen += 1
        fac = golden.get("faction", {}).get(rid, {})
        theirs = next(iter(fac.items()), ("", ""))
        ours_f = (got.get("faction") or "").lower()
        if theirs[0] != ours_f:
            bad["faction"] += 1
            if len(examples["faction"]) < args.examples:
                examples["faction"].append(f"{rid}: OpenMW {fac}, ours {ours_f or '-'}")
        elif theirs[0] and int(theirs[1]) - 1 != int(got.get("rank", 0)):
            bad["rank"] += 1
            if len(examples["rank"]) < args.examples:
                examples["rank"].append(f"{rid} ({theirs[0]}): OpenMW rank {int(theirs[1]) - 1} (from 0), ours {got.get('rank')}")
        rep = golden.get("rep", {}).get(rid)
        if rep is not None and rep != "nil" and int(float(rep)) != int(got.get("reputation", 0)):
            bad["reputation"] += 1
            if len(examples["reputation"]) < args.examples:
                examples["reputation"].append(f"{rid}: OpenMW {rep}, ours {got.get('reputation')}")
    print(f"\nNPC factions: {seen} NPCs; differ in " + (", ".join(f"{f} {n}" for f, n in bad.most_common()) or "nothing"))
    for f, _ in bad.most_common():
        for e in examples[f]:
            print(f"        {e}")
    return sum(bad.values())


def compare_gmsts(golden, data, args):
    """The numeric game settings the engine reads: OpenMW's value beside what the engine uses (the data's, else the
    default its source gives). A setting the data lacks whose default differs from OpenMW's is the one to fix."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from golden_openmw import gmst_names
    defaults = gmst_names()
    data_g = {k.lower(): v for k, v in load_packed(Path(data) / "game_gmst.json").items()} \
        if (Path(data) / "game_gmst.json").exists() else {}
    if "gmst" in data_g and isinstance(data_g["gmst"], dict):
        data_g = {k.lower(): v for k, v in data_g["gmst"].items()}
    bad, missing_ok, rows = 0, 0, []
    for name, theirs in sorted(golden.get("gmst", {}).items()):
        if theirs == "nil":
            rows.append(f"{name}: OpenMW has none; ours {data_g.get(name, sorted(defaults.get(name, [])) or 'no default')}")
            bad += 1
            continue
        t = float(theirs)
        if name in data_g:
            ours, where = data_g[name], "data"
            try:
                ok = abs(float(ours) - t) <= 1e-4 * max(1.0, abs(t))
            except (TypeError, ValueError):
                ok = False
        else:
            ds = defaults.get(name, set())
            ours, where = sorted(ds), "default"
            ok = bool(ds) and all(abs(d - t) <= 1e-4 * max(1.0, abs(t)) for d in ds)
            missing_ok += ok
        if not ok:
            bad += 1
            rows.append(f"{name}: OpenMW {theirs}, ours {ours} ({where})")
    print(f"\nGame settings: {len(golden.get('gmst', {}))} the engine reads; {bad} differ")
    for r in rows:
        print(f"        {r}")
    return bad


def compare_cells(golden, log, args):
    """Every cell's objects and doors: OpenMW's beside the engine's (DUMPCELLS)."""
    eng_obj = defaultdict(dict)
    for cell, oid, n in engine_lines(log, "cellobj"):
        if oid.lower() != "northmarker":       # (the map's north: OpenMW drops it on loading)
            eng_obj[cell][oid.lower()] = int(n)
    theirs = golden.get("cellobj", {})
    only_t = sorted(set(theirs) - set(eng_obj))
    only_o = sorted(set(eng_obj) - set(theirs))
    cell_bad, obj_missing, obj_extra, examples = 0, Counter(), Counter(), []
    for cell in sorted(set(theirs) & set(eng_obj)):
        t, o = theirs[cell], eng_obj[cell]
        if t != o:
            cell_bad += 1
            for i in set(t) | set(o):
                d = t.get(i, 0) - o.get(i, 0)
                if d > 0:
                    obj_missing[i] += d
                elif d < 0:
                    obj_extra[i] -= d
            if len(examples) < args.examples:
                diff = {i: (t.get(i, 0), o.get(i, 0)) for i in set(t) | set(o) if t.get(i, 0) != o.get(i, 0)}
                examples.append(f"{cell}: (OpenMW, ours) {dict(list(diff.items())[:6])}")
    print(f"\nCells: OpenMW {len(theirs)}, ours {len(eng_obj)}; only OpenMW's {len(only_t)} {only_t[:5]}, only ours "
          f"{len(only_o)} {only_o[:5]}; {cell_bad} cells hold different objects")
    for e in examples:
        print(f"        {e}")
    print(f"  most often missing from ours: {obj_missing.most_common(8)}")
    print(f"  most often extra in ours: {obj_extra.most_common(8)}")

    def grid(pos):
        x, y = (float(v) for v in pos.split(",")[:2])
        return f"ext {int(x // 8192)},{int(y // 8192)}"

    def near(a, b, tol=2.0):
        return all(abs(float(p) - float(q)) <= tol for p, q in zip(a.split(","), b.split(",")))

    eng_doors = defaultdict(list)
    for cell, did, pos, dcell, dpos in engine_lines(log, "door"):
        eng_doors[(cell, did.lower())].append((pos, dcell.lower(), dpos))
    door_bad, unmatched, dex = 0, 0, []
    for cell, did, pos, dcell, dpos in golden.get("doors", []):
        cands = [e for e in eng_doors.get((cell, did.lower()), []) if near(e[0], pos)]
        if not cands:
            unmatched += 1
            if len(dex) < args.examples:
                dex.append(f"{cell} {did} at {pos}: not among ours")
            continue
        _, odcell, odpos = cands[0]
        odcell = odcell or grid(odpos)
        if not odcell.startswith("ext") and dcell.startswith("ext"):
            pass
        if (dcell != odcell and not (dcell.startswith("ext") and grid(odpos) == dcell)) or not near(dpos, odpos):
            door_bad += 1
            if len(dex) < args.examples:
                dex.append(f"{cell} {did}: leads to OpenMW {dcell} {dpos}, ours {odcell} {odpos}")
    print(f"  doors: OpenMW {len(golden.get('doors', []))}, ours {sum(len(v) for v in eng_doors.values())}; "
          f"{unmatched} of OpenMW's not found in ours, {door_bad} lead somewhere else")
    for e in dex:
        print(f"        {e}")
    return cell_bad + door_bad + unmatched + len(only_t)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("topic", choices=["actors", "spells", "effects", "items", "leveled", "cspells", "alchemy", "signs", "equipment", "factions", "gmst", "cells", "magic", "world"])
    ap.add_argument("--log", default=str(ROOT / "build" / "native-golden" / "sd" / "3ds" / "mw3ds" / "log.txt"),
                    help="spells: the native run's log with the DUMPSPELLS lines")
    ap.add_argument("--data", default="", help="the converted data folder (default: out/world; with the add-ons "
                    "out/data-tribunal-bloodmoon, as the CIA build names it)")
    ap.add_argument("--esm", action="store_true", help="the converter's NPC numbers from the records, no conversion")
    ap.add_argument("--golden", default="", help="OpenMW's answers (default: build/golden/openmw-actors.json, "
                    "openmw-actors-tribunal-bloodmoon.json with the add-ons)")
    golden_openmw.addon_args(ap)
    ap.add_argument("--examples", type=int, default=5, help="examples listed per field")
    ap.add_argument("--field", default="", help="only these fields (comma separated)")
    args = ap.parse_args()
    # (the add-ons: the converters' modules read the switches too, load_from_esm and npcs_with_leveled_items)
    golden_openmw.set_addons(args)
    sfx = golden_openmw.suffix()
    args.data = args.data or str(ROOT / "out" / ("data" + sfx if sfx else "world"))
    args.golden = args.golden or str(ROOT / "build" / "golden" / f"openmw-actors{sfx}.json")
    golden = json.loads(Path(args.golden).read_text())
    conv = load_from_esm() if args.esm else load_converted(args.data)
    only = set(filter(None, args.field.split(",")))
    if args.topic == "world":
        sys.exit(1 if compare_world_records(golden, args.data, args) else 0)
    if args.topic == "magic":
        sys.exit(1 if compare_magic_records(golden, args.data, args) else 0)
    if args.topic == "equipment":
        sys.exit(1 if compare_equipment(golden, load_converted(args.data)["npc"], args) else 0)
    if args.topic == "factions":
        sys.exit(1 if compare_factions(golden, load_converted(args.data)["npc"], args) else 0)
    if args.topic == "gmst":
        sys.exit(1 if compare_gmsts(golden, args.data, args) else 0)
    if args.topic == "cells":
        sys.exit(1 if compare_cells(golden, args.log, args) else 0)
    if args.topic == "leveled":
        sys.exit(1 if compare_leveled(golden, args.data, args.log, args) else 0)
    if args.topic == "cspells":
        sys.exit(1 if compare_creature_spells(golden, args.log, args) else 0)
    if args.topic == "alchemy":
        sys.exit(1 if compare_alchemy(golden, args.data, args) else 0)
    if args.topic == "signs":
        sys.exit(1 if compare_signs(golden, args.data, args) else 0)
    if args.topic == "items":
        sys.exit(1 if compare_items(golden, args.data, args) else 0)
    if args.topic == "effects":
        sys.exit(1 if compare_effects(golden, args.data, args) else 0)
    if args.topic == "spells":
        sys.exit(1 if compare_spells(golden, load_engine_spells(args.log), conv["npc"], args) else 0)
    n = compare("NPCs", golden["npc"], conv["npc"],
                lambda w: "autocalc (stats from race, class, level)" if w["autocalc"] else "stored stats", args, only)
    if not args.esm:
        n += compare("Creatures", golden["creature"], conv["creature"], lambda w: "creatures", args, only)
    sys.exit(1 if n else 0)


if __name__ == "__main__":
    main()
