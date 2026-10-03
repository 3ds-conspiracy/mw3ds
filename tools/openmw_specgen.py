"""Spec-driven property tests (internal research notes, B).

spec/*.md says each mechanic's formula, with OpenMW's source as the reference. This file is the same spec as
an oracle: it computes the number the formula gives, and writes tests that set the inputs in the engine (random
stats, random effect lists) and EXPECT that exact number. It reads no OpenMW code at run time: the formulas
are written out here in our own words, and the data (effect base costs, flags, ingredients, apparatus quality,
GMSTs) is what the engine loaded (out/world), so a failure is the engine's logic, not the data.

  python tools/openmw_specgen.py [--seed N] [--count N]   writes tools/tests/openmw-spec-magic.txt, openmw-spec-enchant.txt, openmw-spec-alchemy.txt
  tools\\sweep-par.ps1 with test:openmw-spec-magic ... ; each RESULT line counts the mismatches
Every EXPECT is a pair of bounds around the oracle value (a rounding at exactly .5 may fall either way).
"""
import argparse
import collections
import math
import random
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import specdata  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tools" / "tests"

# magic effect flags (the ESM's)
TARGET_SKILL, TARGET_ATTR, NO_DURATION, NO_MAGNITUDE, HARMFUL = 0x1, 0x2, 0x4, 0x8, 0x10
CAST_SELF, CAST_TOUCH, CAST_TARGET, SPELLMAKING, ENCHANTING, APPLIED_ONCE = 0x40, 0x80, 0x100, 0x200, 0x400, 0x1000
SCHOOLS = ["alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration"]
ATTRS = ["strength", "intelligence", "willpower", "agility", "speed", "endurance", "personality", "luck"]
SKILLS = ["block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade", "axe", "spear", "athletics",
          "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration", "alchemy",
          "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman", "mercantile",
          "speechcraft", "handtohand"]


class Data:
    def __init__(self):
        g = specdata.load("game.json")
        self.effects = {int(k): v for k, v in g["magic_effects"].items()}
        self.gmst = specdata.load("game_gmst.json")["gmst"]
        self.objects = {}
        for i in range(10):
            try:
                self.objects.update(specdata.load(f"game_objects_{i}.json")["objects"])
            except FileNotFoundError:
                break

    def f(self, name, default=None):
        v = self.gmst.get(name.lower(), default)
        return float(v)


def round_half_up(x):
    return math.floor(x + 0.5)


def bounds(v, eps=0.01, integer=True):
    """The (low, high) an engine value may take around the oracle's v: an integer result rounded either way
    when v is within eps of a rounding boundary."""
    if integer:
        return round_half_up(v - eps), round_half_up(v + eps)
    return v - eps, v + eps


class Test:
    def __init__(self):
        self.lines = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 GOD"]

    def step(self, tok, secs=0.3):
        self.lines.append(f"{secs} 0 0 0 0 {tok}")

    def wait(self, secs):
        self.lines.append(f"{secs} 0 0 0")

    def expect(self, kind, lo, hi):
        fmt = lambda v: str(int(v)) if float(v).is_integer() else f"{v:.4f}"     # (%g would keep 6 digits)
        if lo == hi:
            self.step(f"EXPECT:{kind}:eq:{fmt(lo)}", 0.1)
        else:
            self.step(f"EXPECT:{kind}:ge:{fmt(lo)}", 0.1)
            self.step(f"EXPECT:{kind}:le:{fmt(hi)}", 0.1)

    def save(self, path):
        (TESTS / path).write_text("\n".join(self.lines) + "\n")
        return len(self.lines)


# ---- Magic: spell cost and cast chance (OpenMW: mwmechanics/spellutil.cpp calcEffectCost, calcSpellBaseSuccessChance)

def effect_cost_made(d, e):
    """Magicka cost of one effect of a spell being made (a duration offset of 1, an area of at least 1;
    the target x1.5 is applied to the running total by spell_cost_made_raw). e = dict(effect, min, max, dur, range, area)."""
    me = d.effects[e["effect"]]
    fl = me["flags"]
    mn = 1 if fl & NO_MAGNITUDE else max(1, e["min"])
    mx = 1 if fl & NO_MAGNITUDE else max(1, e["max"])
    dur = 1 if fl & NO_DURATION else e["dur"]
    if not fl & APPLIED_ONCE:
        dur = max(1, dur)
    x = 0.5 * (mn + mx) * 0.1 * me["cost"] * (1 + dur) + 0.05 * max(1, e["area"]) * me["cost"]
    return x * d.f("feffectcostmult")


def spell_cost_made_raw(d, effects):
    """OpenMW's spell creation (vanilla): each effect adds max(1, cost); a target effect multiplies the running total
    by 1.5."""
    y = 0.0
    for e in effects:
        y += max(1.0, effect_cost_made(d, e))
        if e["range"] == 2:
            y *= 1.5
    return y


def spell_cost_made(d, effects):
    return max(1, int(spell_cost_made_raw(d, effects)))


def cast_chance(d, effects, cost, skills, willpower, luck, sound, fatigue_term):
    """The chance to cast a spell: the weakest effect (twice its school's skill less its own success-chance
    cost) gives the skill; (that - the spell's cost + Willpower / 5 + Luck / 10 - Sound) x the fatigue term."""
    y, lowest = 1e30, 0.0
    for e in effects:
        me = d.effects[e["effect"]]
        x = float(e["dur"])
        if not me["flags"] & APPLIED_ONCE:
            x = max(1.0, x)
        x *= 0.1 * me["cost"]
        x *= 0.5 * (e["min"] + e["max"])
        x += e["area"] * 0.05 * me["cost"]
        if e["range"] == 2:
            x *= 1.5
        x *= d.f("feffectcostmult")
        s = 2.0 * skills[SCHOOLS[me["school"]]]
        if s - x < y:
            y, lowest = s - x, s
    chance = (lowest - cost + 0.2 * willpower + 0.1 * luck - sound) * fatigue_term
    return max(0.0, min(100.0, chance))


def random_effect(d, rng, for_spell=True, self_only=False):
    want = SPELLMAKING if for_spell else ENCHANTING
    ids = [i for i, m in d.effects.items() if m["flags"] & want and m["flags"] & (CAST_SELF | CAST_TOUCH | CAST_TARGET)]
    eid = rng.choice(ids)
    me = d.effects[eid]
    fl = me["flags"]
    ranges = [r for r, bit in enumerate((CAST_SELF, CAST_TOUCH, CAST_TARGET)) if fl & bit]
    mn = rng.randint(1, 60)
    e = {"effect": eid, "min": mn, "max": mn + rng.choice([0, 0, rng.randint(1, 40)]),
         "dur": 0 if fl & NO_DURATION else rng.choice([0, 1, 2, 5, 10, 30, 60, 120, 300]),
         "range": rng.choice(ranges), "area": rng.choice([0, 0, 0, 5, 10, 20, 40]), "arg": "-"}
    if fl & NO_MAGNITUDE:
        e["min"] = e["max"] = 1
    if fl & TARGET_SKILL:
        e["arg"] = rng.choice(SKILLS)
    elif fl & TARGET_ATTR:
        e["arg"] = rng.choice(ATTRS)
    return e


def add_effect(t, e):
    t.step(f"ADDEFFECT:{e['effect']}:{e['min']}:{e['max']}:{e['dur']}:{e['range']}:{e['arg']}:{e['area']}", 0.1)


def gen_magic(d, rng, count):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    spells = []
    # 1. the magicka cost of made spells (1-3 effects)
    for k in range(count):
        effects = [random_effect(d, rng) for _ in range(rng.choice([1, 1, 2, 3]))]
        for e in effects:
            add_effect(t, e)
        t.step(f"MAKESPELL:s{k}", 0.1)
        cost = spell_cost_made(d, effects)
        raw = spell_cost_made_raw(d, effects)
        t.expect(f"spellcost:s{k}", max(1, math.floor(raw - 0.01)), max(1, math.floor(raw + 0.01)))
        spells.append((k, effects, cost))
    # 2. the chance to cast each with random stats; Sound lowers it, and comes last (it stays)
    sound = 0
    for rnd in range(6):
        stats = {s: rng.randint(5, 100) for s in SCHOOLS}
        w, luck = rng.randint(10, 100), rng.randint(10, 100)
        for s, v in stats.items():
            t.step(f"SETSKILL:{s}:{v}", 0.1)
        t.step(f"SETATTR:willpower:{w}", 0.1)
        t.step(f"SETATTR:luck:{luck}", 0.1)
        if rnd >= 4:
            sound += 15
            t.step("EFFECT:48:15:99999", 0.1)
        for k, effects, cost in rng.sample(spells, min(len(spells), 12)):
            cc = cast_chance(d, effects, cost, stats, w, luck, sound, 1.25)
            # the engine keeps the whole points (a truncation): within one point below
            t.expect(f"castchance:s{k}", max(0, math.floor(cc - 0.02)), min(100, math.floor(cc + 0.02)))
    return t


# ---- Enchanting: points and chance (OpenMW: mwmechanics/enchanting.cpp getEffectCosts, getEnchantChance)

def enchant_costs(d, effects, cast_type):
    costs, cost = [], 0.0
    for e in effects:
        base = d.effects[e["effect"]]["cost"]
        dur = d.f("fenchantmentconstantdurationmult") if cast_type == 3 else float(e["dur"])
        cost += ((max(1, e["min"]) + max(1, e["max"])) * dur + max(1, e["area"])) * base * d.f("feffectcostmult") * 0.05
        cost = max(1.0, cost)
        if e["range"] == 2:
            cost *= 1.5
        costs.append(cost)
    return costs


def gen_enchant(d, rng, count):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    for k in range(count):
        cast_type = rng.choice([0, 1, 2, 3])          # once, on strike, when used, constant
        effects = [random_effect(d, rng, for_spell=False) for _ in range(rng.choice([1, 1, 2, 3]))]
        if cast_type == 3:
            for e in effects:
                e["range"] = 0
        skill, intel, luck = rng.randint(5, 100), rng.randint(10, 100), rng.randint(10, 100)
        t.step(f"SETSKILL:enchant:{skill}", 0.1)
        t.step(f"SETATTR:intelligence:{intel}", 0.1)
        t.step(f"SETATTR:luck:{luck}", 0.1)
        t.step(f"ENCHTYPE:{cast_type}", 0.1)
        for e in effects:
            add_effect(t, e)
        costs = enchant_costs(d, effects, cast_type)
        points = sum(math.floor(c) for c in costs)
        precise = sum(costs)
        chance = (skill - precise * d.f("fenchantmentchancemult") + 0.2 * intel + 0.1 * luck) * 1.25
        if cast_type == 3:
            chance *= d.f("fenchantmentconstantchancemult")
        t.expect("enchantpoints", points, points)
        t.expect("enchantchance", *bounds(chance, 0.02, integer=False))
        t.step(f"MAKESPELL:junk{k}", 0.1)             # clears the effects for the next case
    return t


# ---- Alchemy: potion strength (OpenMW: mwmechanics/alchemy.cpp updateEffects, applyTools, listEffects)

def apply_tools(flags, value, tools):
    """The apparatus' quality by which tools are there (tools: mortar, alembic, calcinator, retort)."""
    mag, dur, neg = not flags & NO_MAGNITUDE, not flags & NO_DURATION, bool(flags & HARMFUL)
    tool = tools["alembic"] if neg else tools["retort"]
    calc = tools["calcinator"]
    if tool and calc:
        setup = 1
    elif tool:
        setup = 2
    elif calc:
        setup = 3
    else:
        return value
    if setup == 1:
        q = 2 * tool + 3 * calc if neg else (2 * tool + calc if mag and dur else 2 / 3 * (tool + calc) + 0.5)
    elif setup == 2:
        q = 1 + tool if neg else (tool if mag and dur else tool + 0.5)
    else:
        q = calc if mag and dur else calc + 0.5
    return value + q if setup == 3 or not neg else value / q


def shared_effects(ings):
    """The effects two or more ingredients share, in slot order (each ingredient's effects in its own order)."""
    out = []
    for i in range(len(ings) - 1):
        for key in ings[i]["fx"]:
            if key in out:
                continue
            if any(key in ings[j]["fx"] for j in range(i + 1, len(ings))):
                out.append(key)
    return out


def gen_alchemy(d, rng, count):
    ingr = {}
    for oid, o in d.objects.items():
        if o.get("type") == "INGR" and o.get("effects"):
            fx = []
            for eff, sk, at in o["ingr"]:
                fl = d.effects[eff]["flags"] if eff in d.effects else 0
                fx.append((eff, sk if fl & TARGET_SKILL else -1, at if fl & TARGET_ATTR else -1) if eff >= 0 else None)
            ingr[oid] = {"id": oid, "fx": [k for k in fx if k]}
    appa = {}
    for oid, o in d.objects.items():
        if o.get("type") == "APPA":
            appa.setdefault(o.get("subtype", -1), []).append((oid, o.get("quality", 0.0)))
    names = ["mortar", "alembic", "calcinator", "retort"]
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specalch", 1)
    ids = sorted(ingr)
    done = 0
    while done < count:
        n = rng.choice([2, 2, 3, 4])
        picked = [ingr[i] for i in rng.sample(ids, n)]
        effs = shared_effects([p for p in picked])
        if not effs:
            continue
        tools = {}
        t.step("LOAD:specalch", 0.5)
        t.wait(3)
        t.step("SETSKILL:alchemy:100", 0.1)
        skill, intel, luck = 100, rng.randint(10, 100), rng.randint(10, 100)
        t.step(f"SETATTR:intelligence:{intel}", 0.1)
        t.step(f"SETATTR:luck:{luck}", 0.1)
        for k, nm in enumerate(names):
            tools[nm] = 0.0
            if nm == "mortar" or rng.random() < 0.5:
                oid, q = rng.choice(appa[k])
                t.step(f"GIVE:{oid}", 0.1)
                tools[nm] = q
        for p in picked:
            t.step(f"GIVE:{p['id']}:2", 0.1)
        t.step("BREW:" + ":".join(p["id"] for p in picked), 0.3)
        x = (skill + 0.1 * intel + 0.1 * luck) * tools["mortar"] * d.f("fpotionstrengthmult")
        value = int(x * d.f("ialchemymod"))
        t.expect("brewedvalue", int(x * d.f("ialchemymod") - 0.01), int(x * d.f("ialchemymod") + 0.01))
        out = []
        for key in effs:
            me = d.effects[key[0]]
            fl = me["flags"]
            mag = 1.0 if fl & NO_MAGNITUDE else (x / d.f("fpotiont1magmult")) / me["cost"]
            dur = 1.0 if fl & NO_DURATION else (x / d.f("fpotiont1durmult")) / me["cost"]
            if not fl & NO_MAGNITUDE:
                mag = apply_tools(fl, mag, tools)
            if not fl & NO_DURATION:
                dur = apply_tools(fl, dur, tools)
            m, du = round_half_up(mag), round_half_up(dur)
            if m > 0 and du > 0:
                out.append((m, du, mag, dur))
        for i, (m, du, mag, dur) in enumerate(out):
            t.expect(f"brewedmag:{i}", round_half_up(mag - 0.01), round_half_up(mag + 0.01))
            t.expect(f"brewedduration:{i}", round_half_up(dur - 0.01), round_half_up(dur + 0.01))
        done += 1
    return t


# ---- Armor rating (OpenMW: mwclass/npc.cpp getArmorRating, mwclass/armor.cpp getSkillAdjustedArmorRating)

ARMOR_BASE_WEIGHT = [5, 30, 10, 10, 15, 20, 5, 5, 15, 5, 5]      # helm, cuirass, pauldrons, greaves, boots, gauntlets, shield, bracers
ARMOR_SLOT = [0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7]                   # a bracer takes its hand's slot
SLOT_WEIGHT = [0.1, 0.3, 0.1, 0.1, 0.1, 0.1, 0.05, 0.05, 0.1]


def armor_rating(d, worn, skills):
    """worn: ARMO objects (dicts of the game data). Each of nine slots counts the piece there (its armor x the
    skill of its weight class / iBaseArmorSkill; a weightless piece counts its armor as it is), or the
    Unarmored rating for an empty slot; the slots weigh cuirass 0.3, the rest 0.1 or 0.05 (hands)."""
    un = (d.f("funarmoredbase1") * skills["unarmored"]) * (d.f("funarmoredbase2") * skills["unarmored"])
    slots = [un] * 9
    for o in worn:
        t = o["atype"]
        base = ARMOR_BASE_WEIGHT[t]
        cls = ("lightarmor" if o["weight"] <= base * d.f("flightmaxmod") + 0.0005
               else "mediumarmor" if o["weight"] <= base * d.f("fmedmaxmod") + 0.0005 else "heavyarmor")
        slots[ARMOR_SLOT[t]] = o["armor"] if o["weight"] == 0 else o["armor"] * skills[cls] / d.f("ibasearmorskill")
    return sum(SLOT_WEIGHT[k] * slots[k] for k in range(9))


def gen_armor(d, rng, count):
    armors = collections.defaultdict(list)
    for oid, o in d.objects.items():
        if (o.get("type") == "ARMO" and o.get("health", 0) > 0 and 0 <= o.get("atype", -1) <= 10
                and not o.get("ench")):          # (an enchantment can add a Shield effect)
            armors[o["atype"]].append(o)
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specarmor", 1)
    for k in range(count):
        t.step("LOAD:specarmor", 0.5)
        t.wait(3)
        skills = {n: rng.randint(5, 100) for n in ("unarmored", "lightarmor", "mediumarmor", "heavyarmor")}
        for n, v in skills.items():
            t.step(f"SETSKILL:{n}:{v}", 0.1)
        slots_used, worn = set(), []
        for _ in range(rng.randint(0, 9)):
            atype = rng.choice([a for a in armors if a not in (9, 10)] + [9, 10])
            if ARMOR_SLOT[atype] in slots_used or not armors[atype]:
                continue
            slots_used.add(ARMOR_SLOT[atype])
            o = rng.choice(armors[atype])
            worn.append(o)
            t.step(f"GIVE:{o['id'].replace(' ', '%')}", 0.1)
        v = armor_rating(d, worn, skills)
        t.expect("armor", *bounds(v, 0.05, integer=False))
    return t


# ---- Formula pieces of formulas.cpp: repair, recharge, security, persuasion, resistance, crime, enchanted casts
# (spec/combat.md, spec/derived.md, spec/crime.md; OpenMW: repair.cpp, recharge.cpp, security.cpp, dialoguemanagerimp /
# mechanicsmanagerimp.cpp getPersuasionChance, spellresistance.cpp, mechanicsmanagerimp.cpp reportCrime, spellutil.cpp)

def fatigue_term(d, frac):
    return d.f("ffatiguebase", 1.25) - d.f("ffatiguemult", 0.5) * (1.0 - frac)


def rel(v, tol=1e-4, floor=0.02):
    """bounds for a float result: a relative tolerance with a small absolute floor"""
    e = max(floor, abs(v) * tol)
    return v - e, v + e


def trunc_bounds(v, eps=0.01):
    """bounds for an int(...) of a float that may land a hair below a whole number"""
    return math.floor(v - eps), math.floor(v + eps)


def setup_stats(t, stats, frac):
    """stats: {attribute or skill name: value}. SETFATIGUE goes last: changing an attribute refills the bars."""
    for n, v in stats.items():
        t.step(f"SETATTR:{n}:{v}" if n in ATTRS else f"SETSKILL:{n}:{v}", 0.1)
    t.step(f"SETFATIGUE:{frac}", 0.1)


def new_stats_test(count, slot):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step(f"SAVE:{slot}", 1)
    return t


def load(t, slot):
    t.step(f"LOAD:{slot}", 0.5)
    t.wait(3)


def gen_repair(d, rng, count):
    t = new_stats_test(count, "specrepair")
    for k in range(count):
        load(t, "specrepair")
        st = {"strength": rng.randint(5, 99), "luck": rng.randint(5, 99), "armorer": rng.randint(5, 100)}
        frac = rng.choice([1.0, 0.75, 0.5, 0.25, 0.1])
        setup_stats(t, st, frac)
        x = (0.1 * st["strength"] + 0.1 * st["luck"] + st["armorer"]) * fatigue_term(d, frac)
        t.expect("repairchance", *rel(x))
        q = rng.choice([0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0])       # (binary fractions: the product is exact)
        roll = rng.randint(0, 99)
        amount = max(1, int(d.f("frepairamountmult", 3.0) * q * roll))
        t.expect(f"repairamount:{q},{roll}", amount, amount)
    return t


def gen_recharge(d, rng, count):
    t = new_stats_test(count, "specrecharge")
    for k in range(count):
        load(t, "specrecharge")
        st = {"enchant": rng.randint(5, 100), "intelligence": rng.randint(5, 100), "luck": rng.randint(1, 99)}
        frac = rng.choice([1.0, 0.75, 0.5, 0.25, 0.1])
        setup_stats(t, st, frac)
        luck = 0.1 * st["luck"]
        luck = 1.0 if luck < 1.0 or luck > 10.0 else luck
        intel = max(1.0, min(20.0, 0.2 * st["intelligence"]))
        x = (st["enchant"] + intel + luck) * fatigue_term(d, frac)
        t.expect("rechargechance", *rel(x))
        soul, roll = rng.choice([30, 60, 150, 400, 1000]), rng.randint(0, 99)
        t.expect(f"rechargegain:{soul},{roll}", *rel(soul * roll / x, 1e-3, 0.05))
    return t


def gen_security(d, rng, count):
    t = new_stats_test(count, "specsecurity")
    for k in range(count):
        load(t, "specsecurity")
        st = {"agility": rng.randint(5, 99), "luck": rng.randint(5, 99), "security": rng.randint(5, 100)}
        frac = rng.choice([1.0, 0.75, 0.5, 0.25, 0.1])
        setup_stats(t, st, frac)
        ft = fatigue_term(d, frac)
        base = 0.2 * st["agility"] + 0.1 * st["luck"] + st["security"]
        q = rng.choice([0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0])
        level = rng.randint(0, 100)
        t.expect(f"lockchance:{level},{q}", *rel(base * q * ft + d.f("fpicklockmult", -1.0) * level))
        cost = rng.choice([0, 5, 10, 25, 40, 60])
        t.expect(f"trapchance:{cost},{q}", *rel((base + d.f("ftrapcostmult", -1.0) * cost) * q * ft))
    return t


PERSUADE_NPCS = [("arrille", "Seyda_Neen,_Arrille's_Tradehouse"), ("aryon", "Tel_Vos,_Aryon's_Chambers"),
                 ("dratha", "Tel_Mora,_Upper_Tower"), ("neloth", "Sadrith_Mora,_Tel_Naga_Upper_Tower")]


def load_actors():
    out = {}
    for name in ["game_actors.json"] + [f"game_actors_{i}.json" for i in range(20)]:
        try:
            for a in specdata.load(name)["actors"]:
                out[a["id"].lower()] = a
        except FileNotFoundError:
            continue
    return out


def gen_persuasion(d, rng, count):
    actors = load_actors()
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specpersuade", 1)
    g = d.f
    for k in range(count):
        npc, cell = rng.choice([n for n in PERSUADE_NPCS if n[0] in actors])
        a = actors[npc]
        t.step("LOAD:specpersuade", 0.5)
        t.wait(3)
        t.step(f"GOTO:{cell}", 0.3)
        t.wait(8)
        st = {"personality": rng.randint(5, 99), "luck": rng.randint(5, 99), "speechcraft": rng.randint(5, 100),
              "mercantile": rng.randint(5, 100)}
        rep = rng.choice([0, 3, 10, 40])
        frac = rng.choice([1.0, 0.75, 0.5, 0.25])
        disp = rng.choice([50, 40, 65, 25, 90])
        setup_stats(t, st, frac)
        t.step(f"SETREP:{rep}", 0.1)
        t.step(f"SETDISP:{npc}:{disp}", 0.1)
        ftp = fatigue_term(d, frac)
        pers_p, luck_p = st["personality"] / g("fpersonalitymod", 5.0), st["luck"] / g("fluckmod", 10.0)
        rep_p, level_p = rep * g("freputationmod", 1.0), 1 * g("flevelmod", 5.0)       # (a new character is level 1)
        p1 = (rep_p + luck_p + pers_p + st["speechcraft"]) * ftp
        p2 = p1 + level_p
        p3 = (st["mercantile"] + luck_p + pers_p) * ftp
        an, sk = a["attributes"], a["skills"]
        pers_n, luck_n = an[6] / g("fpersonalitymod", 5.0), an[7] / g("fluckmod", 10.0)
        rep_n, level_n = a.get("reputation", 0) * g("freputationmod", 1.0), a.get("level", 1) * g("flevelmod", 5.0)
        ftn = 1.25          # (an NPC starts a scene rested)
        n1 = (rep_n + luck_n + pers_n + sk[25]) * ftn
        n2 = (level_n + rep_n + luck_n + pers_n + sk[25]) * ftn
        n3 = (sk[24] + rep_n + luck_n + pers_n) * ftn
        dd = 1.0 - 0.02 * abs(disp - 50)
        for idx, v in enumerate((p1, p2, p3, n1, n2, n3, dd)):
            t.expect(f"persuadepart:{npc},{idx}", *rel(v, 1e-3, 0.05))
        bribe = [g("fbribe10mod", 35.0), g("fbribe100mod", 75.0), g("fbribe1000mod", 150.0)]
        raw = {0: dd * (p1 - n1 + 50), 1: dd * (p2 - n2 + 50), 2: dd * (p1 - n1 + 50)}
        for b in range(3):
            raw[3 + b] = dd * (p3 - n3 + 50) + bribe[b]
        for kind in range(6):
            t.expect(f"persuadechance:{npc},{kind}", *rel(max(g("iperminchance", 5.0), raw[kind]), 1e-3, 0.05))
    return t


# OpenMW's getResistanceEffect / getWeaknessEffect (loadmgef.cpp), by our effect ids
RESIST_MAGICKA, WEAK_MAGICKA = 93, 31
RESIST_MAP = {14: (90, 28), 16: (91, 29), 15: (92, 30), 27: (97, 35), 45: (99, None), 133: (94, 32), 132: (96, 34)}
for e in list(range(17, 27)) + list(range(28, 37)) + list(range(49, 57)) + list(range(85, 90)) + [7, 44, 46, 47, 48, 101]:
    RESIST_MAP[e] = (RESIST_MAGICKA, WEAK_MAGICKA)
SHIELD_OF = {14: 4, 16: 6, 15: 5}


def gen_resist(d, rng, count):
    t = new_stats_test(count, "specresist")
    resist_ids = sorted({r for r, _ in RESIST_MAP.values()})
    weak_ids = sorted({w for _, w in RESIST_MAP.values() if w is not None})
    for k in range(count):
        load(t, "specresist")
        st = {"willpower": rng.randint(5, 99), "luck": rng.randint(5, 99)}
        frac = rng.choice([1.0, 0.75, 0.5, 0.25, 0.1])
        setup_stats(t, st, frac)
        # (ACTIVE puts an effect on the player at once, with no roll of its own)
        active = collections.Counter()
        for _ in range(rng.randint(0, 4)):
            eff = rng.choice(resist_ids + weak_ids + [4, 5, 6])
            m = rng.randint(5, 60)
            active[eff] += m
            t.step(f"ACTIVE:{eff}:{m}", 0.1)
        for eff in rng.sample(sorted(RESIST_MAP) + [9, 75, 80], 6):
            res, weak = RESIST_MAP.get(eff, (None, None))
            v = 0.0
            if res is not None:
                v = active[res] - (active[weak] if weak is not None else 0)
                if eff in SHIELD_OF:
                    v += active[SHIELD_OF[eff]]
            t.expect(f"resistbase:{eff}", *rel(v))
        x = (st["willpower"] + 0.1 * st["luck"]) * fatigue_term(d, frac) * 0.5
        t.expect("resistx", *rel(x))
    return t


def gen_crime(d, rng, count):
    t = Test()
    # (no world needed: the bounty each kind of crime adds; 0 theft, 1 pickpocket, 2 trespass, 3 assault, 4 murder)
    for k in range(count):
        kind = rng.choice([0, 0, 0, 1, 2, 3, 4])
        value = rng.choice([1, 5, 20, 100, 750, 4000]) if kind == 0 else 0
        if kind == 0:
            b = max(1, int(value * d.f("fcrimestealing", 1.0)))
        else:
            b = int(d.f({1: "icrimepickpocket", 2: "icrimetresspass", 3: "icrimeattack", 4: "icrimekilling"}[kind]))
        t.expect(f"crimebounty:{kind},{value}", b, b)
    return t


def gen_enchcast(d, rng, count):
    t = new_stats_test(count, "specenchcast")
    for k in range(count):
        load(t, "specenchcast")
        skill = rng.randint(5, 100)
        t.step(f"SETSKILL:enchant:{skill}", 0.1)
        for _ in range(4):
            cost = rng.choice([1, 3, 7, 12, 20, 33, 50, 80, 120])
            r = cost - cost / 100.0 * (skill - 10)
            v = max(1.0, r)
            t.expect(f"enchantcastcost:{cost}", *trunc_bounds(v))
    return t


def npc_barter_terms(a, ftn=1.25):
    an, sk = a["attributes"], a["skills"]
    return (min(100, sk[24]) + min(10.0, 0.1 * an[7]) + min(10.0, 0.2 * an[6])) * ftn      # (a rested NPC)


def barter_price(d, a, disp, st, frac, value, buying):
    """OpenMW getBarterOffer: (lo, hi) bounds of the price for the int() truncation"""
    ftp = fatigue_term(d, frac)
    pc = (disp - 50 + min(100, st["mercantile"]) + min(10.0, 0.1 * st["luck"]) + min(10.0, 0.2 * st["personality"])) * ftp
    npc = npc_barter_terms(a)
    term = 0.01 * (100 - 0.5 * (pc - npc)) if buying else 0.01 * (50 - 0.5 * (npc - pc))
    lo, hi = trunc_bounds(value * term)
    return max(1, lo), max(1, hi)


def gen_barter_train_travel(d, rng, count, what):
    actors = load_actors()
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step(f"SAVE:spec{what}", 1)
    g = d.f
    for k in range(count):
        npc, cell = rng.choice([n for n in PERSUADE_NPCS if n[0] in actors])
        a = actors[npc]
        t.step(f"LOAD:spec{what}", 0.5)
        t.wait(3)
        t.step(f"GOTO:{cell}", 0.3)
        t.wait(8)
        st = {"personality": rng.randint(5, 99), "luck": rng.randint(5, 99), "mercantile": rng.randint(5, 100)}
        if what == "training":
            skill = rng.randrange(27)
            st[SKILLS[skill]] = rng.randint(5, 95)
        frac = rng.choice([1.0, 0.75, 0.5, 0.25])
        disp = rng.choice([50, 40, 65, 25, 90, 10])
        setup_stats(t, st, frac)
        t.step(f"SETDISP:{npc}:{disp}", 0.1)
        if what == "barter":
            for _ in range(4):
                value, buying = rng.choice([1, 8, 25, 60, 150, 400, 1200, 5000]), rng.choice([1, 0])
                lo, hi = barter_price(d, a, disp, st, frac, value, buying)
                t.expect(f"barterprice:{npc},{value},{buying}", lo, hi)
            # haggling (OpenMW tradewindow.cpp haggle): whole-number d, uncapped terms
            for _ in range(3):
                price = rng.choice([20, 75, 300, 1000])
                selling = rng.choice([0, 1])
                offer = int(price * (rng.choice([1.05, 1.2, 1.5]) if selling else rng.choice([0.95, 0.8, 0.5])))
                dd = int(100 * (offer - price) / offer) if selling else int(100 * (price - offer) / price)
                pc = (g("fdispositionmod", 1.0) * (min(100, max(0, disp)) - 50) + st["mercantile"] + 0.1 * st["luck"]
                      + 0.2 * st["personality"]) * fatigue_term(d, frac)
                an, sk = a["attributes"], a["skills"]
                npcv = (sk[24] + 0.1 * an[7] + 0.2 * an[6]) * 1.25
                x = g("fbargainoffermulti", -4.0) * dd + g("fbargainofferbase", 50.0) + int(pc - npcv)
                t.expect(f"hagglechance:{npc},{price},{offer},{selling}", *rel(x, 1e-4, 1.01))
        elif what == "training":
            base = max(1, int(st[SKILLS[skill]] * g("itrainingmod", 10.0)))       # (the BASE skill: OpenMW's getBase)
            lo, hi = barter_price(d, a, disp, st, frac, base, 1)
            t.expect(f"trainprice:{npc},{skill}", lo, hi)
        else:
            for _ in range(3):
                dist = rng.choice([500.0, 3000.0, 9000.0, 20000.0, 61234.0, 150000.0])
                followers, interior = rng.choice([0, 0, 1, 3]), rng.choice([0, 0, 1])
                if interior:
                    p = int(g("fmagesguildtravel", 10.0))
                else:
                    mult = g("ftravelmult", 4000.0)
                    p = int(dist / mult) if mult else int(dist)
                p = max(1, p * (1 + followers))
                lo, hi = barter_price(d, a, disp, st, frac, p, 1)
                t.expect(f"travelprice:{npc},{dist},{followers},{interior}", lo, hi)
    return t


def gen_barter(d, rng, count):
    return gen_barter_train_travel(d, rng, count, "barter")


def gen_training(d, rng, count):
    return gen_barter_train_travel(d, rng, count, "training")


def gen_travel(d, rng, count):
    return gen_barter_train_travel(d, rng, count, "travel")


# ---- Combat (spec/combat.md; OpenMW combat.cpp getHitChance / blockMeleeAttack / applyElementalShields,
# character.cpp getFallDamage, data-mw combat/local.lua applyStagger)

def fatigue_term_of(d, cur, mx):
    norm = 1.0 if math.floor(mx) == 0 else max(0.0, cur / mx)
    return d.f("ffatiguebase", 1.25) - d.f("ffatiguemult", 0.5) * (1 - norm)


def gen_combat(d, rng, count):
    t = new_stats_test(count, "speccombat")
    g = d.f
    for k in range(count):
        load(t, "speccombat")
        st = {"agility": rng.randint(5, 99), "luck": rng.randint(5, 99), "acrobatics": rng.randint(5, 100)}
        frac = rng.choice([1.0, 0.75, 0.5, 0.25, 0.1])
        setup_stats(t, st, frac)
        ft = fatigue_term(d, frac)
        skill = rng.randint(5, 100)
        t.expect(f"attackterm:{skill}", *rel((skill + st["agility"] / 5 + st["luck"] / 10) * ft))
        t.expect("playerdefense", *rel((st["agility"] / 5 + st["luck"] / 10) * ft))
        agi = rng.randint(5, 100)
        t.expect(f"knockodds:{agi}", *rel(agi * g("iknockdownoddsmult", 50) * 0.01 + g("iknockdownoddsbase", 50)))
        h = rng.choice([100.0, 399.0, 400.0, 450.0, 600.0, 900.0, 1500.0, 3000.0])
        acro = st["acrobatics"]
        mn = g("ffalldamagedistancemin", 400)
        if h < mn:
            fd = 0.0
        else:
            x = max(0.0, h - mn - 1.5 * acro)
            fd = (g("ffalldistancebase", 0) + g("ffalldistancemult", 0.07) * x) * (g("ffallacrobase", 0.5) + g("ffallacromult", 0.015) * (100 - acro))
        t.expect(f"falldamage:{h}", *rel(fd))
        # block: every argument random
        bs, ba, bl, bf, still = rng.randint(5, 100), rng.randint(5, 100), rng.randint(5, 100), rng.choice([1.0, 0.5, 0.2]), rng.choice([0, 1])
        as_, aa, al, af, swing = rng.randint(5, 100), rng.randint(5, 100), rng.randint(5, 100), rng.choice([1.0, 0.5, 0.2]), rng.choice([0.0, 0.25, 0.5, 1.0])
        bt = (bs + 0.2 * ba + 0.1 * bl) * (swing * g("fswingblockmult", 1.0) + g("fswingblockbase", 1.0))
        if still:
            bt *= g("fblockstillbonus", 1.25)
        bt *= fatigue_term_of(d, bf * 100, 100)
        at = (as_ + 0.2 * aa + 0.1 * al) * fatigue_term_of(d, af * 100, 100)
        x = int(bt - at)                  # (C++ truncates toward 0)
        x = max(int(g("iblockminchance", 10)), min(int(g("iblockmaxchance", 50)), x))
        t.expect(f"blockchance:{bs},{ba},{bl},{bf},{still},{as_},{aa},{al},{af},{swing}", *trunc_bounds_c(bt - at, g))
        # elemental shield
        mag, de, wi, lu, ef, res, roll = rng.randint(5, 80), rng.randint(5, 100), rng.randint(5, 100), rng.randint(5, 100), rng.choice([1.0, 0.5]), rng.choice([0, 25, 50]), rng.randint(0, 99)
        save = (de + 0.2 * wi + 0.1 * lu) * 1.25 * ef
        xx = min(100.0, max(0.0, save - roll) + res)
        t.expect(f"elemshield:{mag},{de},{wi},{lu},{ef},{res},{roll}", *rel(g("felementalshieldmult", 0.1) * mag * (1 - 0.01 * xx)))
    return t


def trunc_bounds_c(v, g, eps=0.01):
    lo_c, hi_c = int(g("iblockminchance", 10)), int(g("iblockmaxchance", 50))
    vals = {max(lo_c, min(hi_c, int(v + e))) for e in (-eps, 0, eps)}
    return min(vals), max(vals)


# ---- Movement (spec/movement.md; OpenMW npc.cpp getWalkSpeed / getRunSpeed / getJump, creature.cpp)

def gen_movement(d, rng, count):
    t = Test()
    g = d.f
    for k in range(count):
        sp, ath = rng.randint(0, 100), rng.randint(0, 100)
        walk = g("fminwalkspeed", 100) + 0.01 * sp * (g("fmaxwalkspeed", 200) - g("fminwalkspeed", 100))
        run = walk * (0.01 * ath * g("fathleticsrunbonus", 1.0) + g("fbaserunmultiplier", 1.75))
        t.expect(f"runspeedfor:{sp},{ath}", *rel(run))
        acro, jump, load, running, ft = rng.randint(0, 100), rng.choice([0, 5, 20]), rng.choice([0.0, 0.25, 0.5, 1.0, 1.2]), rng.choice([0, 1]), rng.choice([1.25, 1.0, 0.8])
        if load > 1.0:
            js = 0.0
        else:
            a, b = (acro, 0.0) if acro <= 50 else (50.0, acro - 50.0)
            m = g("fjumpacromultiplier", 4.0)
            x = g("fjumpacrobaticsbase", 128.0) + (a / 15.0) ** m + 3 * b * m + jump * 64
            x *= g("fjumpencumbrancebase", 0.5) + g("fjumpencumbrancemultiplier", 0.5) * (1 - min(1.0, load))
            if running:
                x *= g("fjumprunmultiplier", 1.0)
            x *= ft
            js = (x + 8.96 * 69.99125) / 3
        t.expect(f"jumpspeedfor:{acro},{jump},{load},{running},{ft}", *rel(js, 1e-4, 0.05))
    return t


# ---- NPC AI (spec/npc-ai-behaviour.md; OpenMW aicombataction.cpp vanillaRateFlee, combat.cpp getFightTerm)

def f32(x):
    """x rounded to a C float (OpenMW works the fight term out in floats; a GMST is one)"""
    return struct.unpack("f", struct.pack("f", x))[0]


def gen_ai(d, rng, count):
    actors = load_actors()
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specai", 1)
    g = d.f
    for k in range(count):
        npc, cell = rng.choice([n for n in PERSUADE_NPCS if n[0] in actors])
        a = actors[npc]
        t.step("LOAD:specai", 0.5)
        t.wait(3)
        t.step(f"GOTO:{cell}", 0.3)
        t.wait(8)
        disp = rng.choice([50, 40, 65, 25, 90, 10])
        t.step(f"SETDISP:{npc}:{disp}", 0.1)
        for _ in range(3):
            dist = rng.choice([0.0, 100.0, 500.0, 1500.0, 3000.0, 6000.0])
            # (getFightDistanceBias + getFightDispositionBias, each a float; C++ truncates toward 0)
            bias = f32(f32(g("ifightdistancebase", 20)) - f32(f32(g("ffightdistancemultiplier", 0.005)) * f32(dist)))
            bias = f32(bias + f32(f32(50 - disp) * f32(g("ffightdispmult", 0.2))))
            fight = a.get("fight", 30) + int(bias)
            t.expect(f"fightterm:{npc},{dist}", fight, fight)
            flee = a.get("flee", 0)
            if flee >= 100:
                fr = float(flee)
            else:
                fr = flee * g("faifleefleemult", 0.3)        # (full health)
                if fr != 0:
                    fr += g("ifightdistancebase", 20) - g("ffightdistancemultiplier", 0.005) * dist
            t.expect(f"fleerating:{npc},{dist}", *rel(fr))
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260928)
    ap.add_argument("--count", type=int, default=40)
    args = ap.parse_args()
    d = Data()
    rng = random.Random(args.seed)
    for name, fn, n in (("openmw-spec-magic", gen_magic, args.count), ("openmw-spec-enchant", gen_enchant, args.count),
                        ("openmw-spec-alchemy", gen_alchemy, max(10, args.count // 2)),
                        ("openmw-spec-armor", gen_armor, max(10, args.count // 2)),
                        ("openmw-spec-repair", gen_repair, args.count), ("openmw-spec-recharge", gen_recharge, args.count),
                        ("openmw-spec-security", gen_security, args.count), ("openmw-spec-persuasion", gen_persuasion, 12),
                        ("openmw-spec-combat", gen_combat, args.count),
                        ("openmw-spec-movement", gen_movement, args.count),
                        ("openmw-spec-ai", gen_ai, 12),
                        ("openmw-spec-barter", gen_barter, 12), ("openmw-spec-training", gen_training, 12),
                        ("openmw-spec-travel", gen_travel, 12),
                        ("openmw-spec-resist", gen_resist, args.count), ("openmw-spec-crime", gen_crime, args.count),
                        ("openmw-spec-enchcast", gen_enchcast, max(10, args.count // 2))):
        lines = fn(d, rng, n).save(name + ".txt")
        print(f"{name}: {lines} steps")


if __name__ == "__main__":
    main()
