"""Spec-driven tests for the mechanics of individual magic effects (spec/magic-effects.md).

Same idea as openmw_specgen.py (which this imports and does not change): the numbers below are what OpenMW's
mwmechanics/spelleffects.cpp, activespells.cpp and spellresistance.cpp give, written out here in our own words.
Each case reloads a saved character, sets a few stats, makes a one-effect spell with ADDEFFECT / MAKESPELL, casts it
on the player (CAST) or on a scamp (CASTAT) and EXPECTs the result, mostly relative to a SNAP taken before.

  python tools/test/specgen_effects.py [--seed N] [--count N]    writes tools/test/cases/openmw-spec-effects.txt

What a run can and cannot see: see "Hooks needed" in spec/findings/magic-effects.md. Everything asserted here uses
only EXPECT kinds that exist today: attr, skill, health, healthmax, magicka, magickamax, effect, armor, count,
refhealth, refmagicka, refcombat, refflee.

Tolerances. The resistance roll (spellresistance.cpp getEffectResistance) can take less than 1 percent off a
resistable effect even with no Resist Magicka, because for x > roll the extra resistance is roll / x, below 1.
Resistable magnitudes therefore get a 1.0 percent margin on the low side, and every value half a point of
rounding either way. Per-second effects get a point and a half (the last frame of a duration is partial).
"""
import argparse
import random
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
import openmw_specgen as sg  # noqa: E402

SLOT = "speceffects"
MAX_COST = 40          # a made spell's magicka cost, kept below this so a cast always has the magicka (Int 100)

# attributes and skills the cast itself does not read (cast chance: Willpower, Luck; school skills; resistance)
ATTR_POOL = ["strength", "agility", "speed", "endurance", "personality"]
SKILL_POOL = [s for s in sg.SKILLS if s not in ("destruction", "alteration", "illusion", "conjuration",
                                                 "mysticism", "restoration")]
# attribute / skill effects, by their arg kind
ATTR_EFFECTS = {"drain": 17, "damage": 22, "restore": 74, "fortify": 79, "absorb": 85}
SKILL_EFFECTS = {"drain": 21, "damage": 26, "restore": 78, "fortify": 83, "absorb": 89}


class ETest(sg.Test):
    """sg.Test without god mode (a god-mode player takes no damage) and with snapshot-relative EXPECTs."""

    def __init__(self):
        super().__init__()
        self.lines = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 GOD:0"]
        self.n = 0

    @staticmethod
    def _off(v):
        s = f"{v:.3f}".rstrip("0").rstrip(".")
        return "@" if s in ("0", "-0", "") else "@" + s

    def snap(self, kind, arg=""):
        self.step(f"SNAP:{kind}:{arg}" if arg else f"SNAP:{kind}", 0.1)

    def rel(self, kind, lo, hi):
        """EXPECT kind within [snapshot + lo, snapshot + hi] (kind may carry its argument: 'attr:strength')."""
        if lo > hi:
            lo, hi = hi, lo
        self.step(f"EXPECT:{kind}:ge:{self._off(lo)}", 0.1)
        self.step(f"EXPECT:{kind}:le:{self._off(hi)}", 0.1)

    def absolute(self, kind, lo, hi):
        if lo > hi:
            lo, hi = hi, lo
        self.step(f"EXPECT:{kind}:ge:{sg_num(lo)}", 0.1)
        self.step(f"EXPECT:{kind}:le:{sg_num(hi)}", 0.1)


def sg_num(v):
    return str(int(v)) if float(v).is_integer() else f"{v:.4f}"


def E(effect, mn, mx, dur, rng=0, arg="-", area=0):
    return {"effect": effect, "min": mn, "max": mx, "dur": dur, "range": rng, "arg": arg, "area": area}


def cost_of(d, e):
    return sg.spell_cost_made(d, [e])


def fit(d, e, limit=MAX_COST):
    """Shrink magnitude, then duration, until the spell is cheap enough to always cast."""
    while cost_of(d, e) > limit and e["max"] > 1:
        e["min"] = e["max"] = max(1, int(e["max"] * 0.8))
    while cost_of(d, e) > limit and e["dur"] > 5:
        e["dur"] -= 1
    return e


def magnitude(e):
    return (e["min"], e["max"])


def harmful_range(m):
    """Effective magnitude of a resistable harmful effect cast with no Resist Magicka: the small roll can shave < 1 %."""
    return m * 0.99 - 0.5, m + 0.5


def exact_range(lo, hi):
    return lo - 0.5, hi + 0.5


# ---- setup and casting

def begin(t, stats=None, scamp=False):
    sg.load(t, SLOT)
    for n, v in {"willpower": 5, "luck": 5, "intelligence": 100, **(stats or {})}.items():
        t.step(f"SETATTR:{n}:{v}" if n in sg.ATTRS else f"SETSKILL:{n}:{v}", 0.1)
    t.step("FILL", 0.1)
    if scamp:
        t.step("GOTO:Balmora,_Guild_of_Mages")
        t.wait(8)
        t.step("PLACE:scamp:250")
        t.wait(1.5)
        t.step("FILL", 0.1)


def cast(t, effects, at=None, settle=2.0):
    """Make a spell of the effects and cast it (on the player, or at a placed actor); returns its id."""
    t.n += 1
    name = f"e{t.n}"
    for e in effects:
        sg.add_effect(t, e)
    t.step(f"MAKESPELL:{name}", 0.1)
    sid = f"mw3ds_test_{name}"
    t.step(f"CASTAT:{at}:{sid}" if at else f"CAST:{sid}", 0.3)
    t.wait(settle)
    return sid


def recast(t, sid, settle=2.0):
    t.step(f"CAST:{sid}", 0.3)
    t.wait(settle)


# ---- attributes and skills: drain, fortify, damage, restore, absorb

def arg_of(kind_table, key, rng):
    """(effect id, our EXPECT kind, the arg, base value) for an attribute or a skill."""
    if kind_table is ATTR_EFFECTS:
        a = rng.choice(ATTR_POOL)
        return kind_table[key], "attr", a
    s = rng.choice(SKILL_POOL)
    return kind_table[key], "skill", s


def gen_drain_fortify(t, d, rng, table):
    """Drain / Fortify with a duration: the value moves by the magnitude while it lasts and comes back after.
    Drain lowers modified value only down to 0 (OpenMW: an attribute's damage is capped when read), fortify has no cap."""
    key = rng.choice(["drain", "fortify"])
    eid, kind, a = arg_of(table, key, rng)
    base = rng.randint(30, 60)
    over = key == "drain" and rng.random() < 0.3
    m = base + rng.randint(1, 15) if over else rng.randint(5, 30)
    e = fit(d, E(eid, m, m, rng.choice([6, 8, 10]), 0, a))
    m = e["max"]
    over = key == "drain" and m >= base
    begin(t, {a: base})
    t.snap(kind, a)
    cast(t, [e])
    k = f"{kind}:{a}"
    if key == "fortify":
        t.rel(k, *exact_range(m, m))
    elif over:
        t.absolute(k, 0, 0)
    else:
        lo, hi = harmful_range(m)
        t.rel(k, -hi, -lo)
    t.wait(e["dur"] + 1.5)
    t.rel(k, 0, 0)


def gen_damage_restore(t, d, rng, table):
    """Damage (instant, whole magnitude, stays) then Restore (instant, takes the damage away, never raises above
    the base). Damage is capped at the current modified value."""
    eid_d, kind, a = arg_of(table, "damage", rng)
    eid_r = table["restore"]
    base = rng.randint(30, 60)
    m = rng.randint(5, 25)
    ed = fit(d, E(eid_d, m, m, 0, 0, a))
    lo_d, hi_d = harmful_range(ed["max"])
    begin(t, {a: base})
    t.snap(kind, a)
    cast(t, [ed])
    k = f"{kind}:{a}"
    t.rel(k, -hi_d, -lo_d)
    t.wait(5)
    t.rel(k, -hi_d, -lo_d)                            # damage stays until restored
    y = rng.choice([ed["max"] + 3, max(1, int(ed["max"] * 0.4))])
    er = fit(d, E(eid_r, y, y, 0, 0, a))
    y = er["max"]
    cast(t, [er])
    # what is left of the damage: between (lo_d - y) and (hi_d - y), never below 0
    t.rel(k, -max(0.0, hi_d - y), -max(0.0, lo_d - y))
    # a Restore with nothing left to restore does not raise the value above its base
    cast(t, [er])
    t.rel(k, -max(0.0, hi_d - 2 * y), -max(0.0, lo_d - 2 * y))


def gen_damage_persecond(t, d, rng, table):
    """Damage Attribute / Skill with a duration is per-second: the total after it ends is magnitude x duration."""
    eid, kind, a = arg_of(table, "damage", rng)
    base = rng.randint(40, 70)
    dur = rng.choice([5, 6, 8, 10])
    m = rng.randint(1, max(1, 30 // dur))
    e = fit(d, E(eid, m, m, dur, 0, a))
    tot = e["max"] * e["dur"]
    lo = tot * 0.99 - 1.5
    begin(t, {a: base})
    t.snap(kind, a)
    cast(t, [e], settle=e["dur"] + 3)
    t.rel(f"{kind}:{a}", -(tot + 1.5), -lo)


def gen_stack(t, d, rng, table):
    """Two different spells on one stat add; each ends on its own; recasting the SAME spell refreshes, never adds."""
    eid, kind, a = arg_of(table, "fortify", rng)
    base = rng.randint(30, 50)
    m1, m2 = rng.randint(5, 15), rng.randint(5, 15)
    e1 = fit(d, E(eid, m1, m1, 6, 0, a))
    e2 = fit(d, E(eid, m2, m2, 16, 0, a))
    m1, m2 = e1["max"], e2["max"]
    k = f"{kind}:{a}"
    begin(t, {a: base})
    t.snap(kind, a)
    s1 = cast(t, [e1], settle=1.5)
    cast(t, [e2], settle=2.0)
    t.rel(k, *exact_range(m1 + m2, m1 + m2))
    t.wait(6)                                           # the short one (6 s) is over, the long one (16 s) is not
    t.rel(k, *exact_range(m2, m2))
    recast(t, s1)                                       # a new cast of the short spell: one m1 again, not two
    t.rel(k, *exact_range(m1 + m2, m1 + m2))
    recast(t, s1)
    t.rel(k, *exact_range(m1 + m2, m1 + m2))
    t.wait(22)
    t.rel(k, 0, 0)


def gen_drain_and_fortify_net(t, d, rng, table):
    """A Drain and a Fortify on the same stat net out: Fortify raises the modifier, Drain adds damage."""
    a = rng.choice(ATTR_POOL if table is ATTR_EFFECTS else SKILL_POOL)
    kind = "attr" if table is ATTR_EFFECTS else "skill"
    base = rng.randint(40, 60)
    f, dr = rng.randint(5, 20), rng.randint(5, 20)
    ef = fit(d, E(table["fortify"], f, f, 12, 0, a))
    ed = fit(d, E(table["drain"], dr, dr, 12, 0, a))
    f, dr = ef["max"], ed["max"]
    begin(t, {a: base})
    t.snap(kind, a)
    cast(t, [ef], settle=1.5)
    cast(t, [ed], settle=2.0)
    lo_d, hi_d = harmful_range(dr)
    t.rel(f"{kind}:{a}", f - hi_d - 0.5, f - lo_d + 0.5)


# ---- health, magicka: damage, restore, drain, fortify, per-second

def gen_health(t, d, rng):
    which = rng.choice(["damage_ps", "poison", "drain", "fortify", "fortify_damage", "damage_restore", "restore_ps"])
    begin(t)
    t.snap("health")
    if which in ("damage_ps", "poison"):
        eid = 23 if which == "damage_ps" else 27
        dur = rng.choice([2, 3, 5, 8])
        m = rng.randint(1, max(1, 30 // dur))
        e = fit(d, E(eid, m, m, dur))
        tot = e["max"] * e["dur"]
        cast(t, [e], settle=e["dur"] + 3)
        t.rel("health", -(tot + 1.5), -(tot * 0.99 - 1.5))
        t.wait(5)
        t.rel("health", -(tot + 1.5), -(tot * 0.99 - 1.5))        # it is over: nothing more
    elif which == "drain":
        m = rng.randint(5, 30)
        e = fit(d, E(18, m, m, rng.choice([6, 8, 10])))
        m = e["max"]
        t.snap("healthmax")
        cast(t, [e])
        lo, hi = harmful_range(m)
        t.rel("health", -hi, -lo)
        t.rel("healthmax", 0, 0)                        # Drain Health lowers the current value only
        t.wait(e["dur"] + 1.5)
        t.rel("health", 0, 0)                           # and gives it back
    elif which in ("fortify", "fortify_damage"):
        m = rng.randint(5, 40)
        e = fit(d, E(80, m, m, rng.choice([6, 8, 10])))
        m = e["max"]
        t.snap("healthmax")
        cast(t, [e])
        t.rel("healthmax", *exact_range(m, m))
        t.rel("health", *exact_range(m, m))             # (the current value rises with the maximum)
        if which == "fortify_damage":
            x = rng.randint(5, 25)
            xe = fit(d, E(23, x, x, 0))
            x = xe["max"]
            cast(t, [xe])
            lo, hi = harmful_range(x)
            t.wait(e["dur"] + 1.5)
            # the fortify ends: the current value drops by m whatever damage was taken (no protection from it)
            t.rel("health", -hi, -lo)
            t.rel("healthmax", 0, 0)
        else:
            t.wait(e["dur"] + 1.5)
            t.rel("healthmax", 0, 0)
            t.rel("health", 0, 0)
    elif which == "damage_restore":
        x = rng.randint(10, 40)
        xe = fit(d, E(23, x, x, 0))
        x = xe["max"]
        cast(t, [xe])
        lo, hi = harmful_range(x)
        t.rel("health", -hi, -lo)
        y = rng.randint(5, 40)
        ye = fit(d, E(75, y, y, 0))
        y = ye["max"]
        cast(t, [ye])
        t.rel("health", -max(0.0, hi - y) - 0.5, -max(0.0, lo - y) + 0.5)
    else:                                               # restore per second
        x = rng.randint(20, 40)
        xe = fit(d, E(23, x, x, 0))
        x = xe["max"]
        cast(t, [xe])
        lo, hi = harmful_range(x)
        dur, y = rng.choice([4, 5, 6, 8]), rng.randint(1, 5)
        ye = fit(d, E(75, y, y, dur))
        tot = ye["max"] * ye["dur"]
        t.snap("health")
        cast(t, [ye], settle=ye["dur"] + 3)
        # the heal is capped by what was lost: between 0 and the damage taken
        t.rel("health", max(0.0, min(tot * 0.99 - 1.5, lo - 0.5)), min(tot + 1.5, hi + 0.5))


def gen_magicka(t, d, rng):
    which = rng.choice(["damage", "restore", "drain", "fortify", "fortify_max"])
    if which == "fortify_max":
        m = rng.randint(10, 60)
        e = fit(d, E(84, m, m, rng.choice([6, 8, 10])))
        m = e["max"]
        intel = rng.randint(60, 100)
        begin(t, {"intelligence": intel})
        t.snap("magickamax")
        cast(t, [e])
        # (creaturestats.cpp recalculateMagicka, not on the reading list: max = int((multiplier + 0.1 x magnitude) x Int))
        gain = 0.1 * m * intel
        t.rel("magickamax", gain - 1.0, gain + 1.0)
        t.wait(e["dur"] + 1.5)
        t.rel("magickamax", 0, 0)
        return
    begin(t)
    t.snap("magicka")
    if which == "damage":
        m = rng.randint(5, 30)
        e = fit(d, E(24, m, m, 0))
        m = e["max"]
        c = cost_of(d, e)
        cast(t, [e])
        lo, hi = harmful_range(m)
        t.rel("magicka", -c - hi - 1.0, -c - lo + 1.0)
    elif which == "restore":
        # a Restore Magicka larger than the cost refills what the cast took: back to full
        e = fit(d, E(76, 60, 60, 0), 30)
        cast(t, [e])
        t.rel("magicka", 0, 0)
    elif which == "drain":
        m = rng.randint(5, 30)
        e = fit(d, E(19, m, m, rng.choice([6, 8, 10])))
        m = e["max"]
        c = cost_of(d, e)
        t.snap("magickamax")
        cast(t, [e])
        lo, hi = harmful_range(m)
        t.rel("magicka", -c - hi - 1.0, -c - lo + 1.0)
        t.rel("magickamax", 0, 0)
        t.wait(e["dur"] + 1.5)
        t.rel("magicka", -c - 1.0, -c + 1.0)            # the drain is given back; the cast's cost is not
    else:
        m = rng.randint(5, 40)
        e = fit(d, E(81, m, m, rng.choice([6, 8, 10])))
        m = e["max"]
        t.snap("magickamax")
        cast(t, [e])
        t.rel("magickamax", *exact_range(m, m))
        t.wait(e["dur"] + 1.5)
        t.rel("magickamax", 0, 0)


# ---- resist / weakness by element (and Resist Magicka)

SHIELD = {14: 4, 15: 5, 16: 6}


def resist_multiplier(eid, active):
    """1 - resistance / 100 for a resistable effect (spellresistance.cpp getEffectResistanceAttribute), the part that
    does not depend on the roll; active: {effect id: magnitude} on the target."""
    res, weak = sg.RESIST_MAP.get(eid, (None, None))
    r = 0.0
    if res is not None:
        r += active.get(res, 0)
    if weak is not None:
        r -= active.get(weak, 0)
    if eid in SHIELD:
        r += active.get(SHIELD[eid], 0)
    return max(0.0, 1.0 - min(r, 100.0) / 100.0), r


def gen_resist(t, d, rng):
    eid = rng.choice([14, 15, 16, 27, 23, 18])           # fire, shock, frost, poison, damage health, drain health
    res_id, weak_id = sg.RESIST_MAP[eid]
    active = {}
    for ef in rng.sample([res_id, weak_id] + ([SHIELD[eid]] if eid in SHIELD else []) + [93, 28, 91], rng.randint(1, 3)):
        if ef is not None:
            active[ef] = rng.choice([10, 25, 50] if 28 <= ef <= 36 else [10, 25, 40, 50, 75, 100, 120])
    begin(t)
    t.snap("health")
    for ef, mag in active.items():
        t.step(f"ACTIVE:{ef}:{mag}", 0.1)
    mult, r = resist_multiplier(eid, active)
    m = rng.randint(8, 20)
    dur = 1 if eid not in (18,) else 8
    e = fit(d, E(eid, m, m, dur))
    m = e["max"]
    cast(t, [e], settle=2.0 if eid != 18 else 3.0)
    if eid == 18:                                        # drain: read while it lasts
        lo = m * (mult - 0.01) - 0.5 if mult > 0 else 0.0
        hi = m * mult + 0.5
        if r < 0:                                        # weakness: magnitude above 1x, bounds widen upward
            hi = m * (1 - r / 100.0) + 0.5
            lo = m * (1 - r / 100.0) * 0.99 - 0.5
        t.rel("health", -hi, -max(0.0, lo))
    else:
        factor = 1.0 - r / 100.0 if r < 0 else mult
        t.rel("health", -(m * factor + 0.5 + 0.5), -max(0.0, m * factor * 0.99 - 0.5))


def scamp_elem(t, d, rng):
    """The scamp's own resistances (const_effects of the converted record) against elemental damage, with a duration:
    loss = magnitude x duration x (1 - resist / 100)."""
    sc = sg.load_actors()["scamp"]
    resists = {e[0]: e[1] for e in sc["const_effects"]}
    eid = rng.choice([14, 15, 16, 27, 23])
    mult, _ = resist_multiplier(eid, resists)
    dur = rng.choice([1, 2, 3])
    m = rng.randint(4, 24 // dur)
    e = fit(d, E(eid, m, m, dur, 2))
    tot = e["max"] * e["dur"] * mult
    begin(t, scamp=True)
    t.snap("refhealth", "scamp")
    cast(t, [e], at="scamp", settle=e["dur"] + 4)
    t.rel("refhealth:scamp", -(tot + 1.5), -max(0.0, tot * 0.99 - 1.5))


# ---- absorb (on a scamp: caster side by attribute / skill; target side by health / magicka)

def gen_absorb(t, d, rng):
    which = rng.choice(["attr", "skill", "health", "magicka"])
    begin(t, scamp=True)
    dur = rng.choice([6, 8])
    if which in ("attr", "skill"):
        table = ATTR_EFFECTS if which == "attr" else SKILL_EFFECTS
        a = rng.choice(ATTR_POOL if which == "attr" else SKILL_POOL)
        m = rng.randint(5, 25)
        e = fit(d, E(table["absorb"], m, m, dur, 2, a))
        m = e["max"]
        begin_stat = rng.randint(30, 50)
        t.step(f"SETATTR:{a}:{begin_stat}" if which == "attr" else f"SETSKILL:{a}:{begin_stat}", 0.1)
        t.snap(which, a)
        cast(t, [e], at="scamp", settle=3.0)
        lo, hi = harmful_range(m)
        # the caster is fortified by the (resisted) magnitude while it lasts; the scamp has no Resist Magicka
        t.rel(f"{which}:{a}", lo - 0.5, hi + 0.5)
        t.wait(e["dur"] + 1.5)
        t.rel(f"{which}:{a}", 0, 0)
    else:
        eid, tk, key = (86, "refhealth", "health") if which == "health" else (87, "refmagicka", "magicka")
        dur = rng.choice([1, 2, 3])
        m = rng.randint(3, 10)
        e = fit(d, E(eid, m, m, dur, 2))
        tot = e["max"] * e["dur"]
        t.snap(tk, "scamp")
        cast(t, [e], at="scamp", settle=e["dur"] + 4)
        t.rel(f"{tk}:scamp", -(tot + 1.5), -(tot * 0.99 - 1.5))


# ---- cure and dispel

def gen_cure_dispel(t, d, rng):
    which = rng.choice(["cure_poison", "dispel_fortify", "dispel_poison"])
    if which == "dispel_fortify":
        a = rng.choice(ATTR_POOL)
        m = rng.randint(5, 20)
        e = fit(d, E(79, m, m, 25, 0, a), 30)
        m = e["max"]
        begin(t, {a: 40})
        t.snap("attr", a)
        cast(t, [e], settle=2.0)
        t.rel(f"attr:{a}", *exact_range(m, m))
        cast(t, [E(57, 100, 100, 0)], settle=2.0)        # magnitude 100: every spell on the target goes
        t.rel(f"attr:{a}", 0, 0)
        return
    begin(t)
    m = rng.randint(1, 2)
    e = E(27, m, m, 30)                                  # (not fitted: it must outlast the cast that ends it)
    cast(t, [e], settle=2.5)
    if which == "cure_poison":
        t.absolute("effect:27", m * 0.99 - 0.5, m + 0.5)
        cast(t, [E(72, 1, 1, 0)], settle=2.0)
        t.absolute("effect:27", 0, 0)
        t.snap("health")
        t.wait(4)
        t.rel("health", -0.5, 0.5)                       # no more poison damage once it is cured
    else:
        cast(t, [E(57, 100, 100, 0)], settle=2.0)
        t.absolute("effect:27", 0, 0)


# ---- effects that only register a magnitude (read by other systems): shield, burden, feather, sanctuary, chameleon...

MOD_EFFECTS = [3, 4, 5, 6, 7, 8, 9, 10, 11, 1, 40, 41, 42, 67, 68, 117,
               28, 29, 30, 31, 35, 90, 91, 92, 93, 94, 97, 99]


def gen_modifier(t, d, rng):
    eid = rng.choice(MOD_EFFECTS)
    harm = eid in sg.RESIST_MAP and eid in (7, 28, 29, 30, 31, 35)     # Resist Magicka can shave < 1 %
    mn = rng.randint(5, 40)
    mx = mn if harm or rng.random() < 0.5 else mn + rng.randint(1, 20)
    d1 = rng.choice([6, 8, 10])
    e1 = fit(d, E(eid, mn, mx, d1))
    e2 = fit(d, E(eid, mn, mn, 20))
    begin(t)
    if eid == 3:
        t.snap("armor")
        key = "armor"
    else:
        t.snap("effect", str(eid))
        key = f"effect:{eid}"
    lo1, hi1 = (harmful_range(e1["min"])[0], e1["max"] + 0.5) if harm else exact_range(e1["min"], e1["max"])
    lo2, hi2 = (harmful_range(e2["min"])[0], e2["max"] + 0.5) if harm else exact_range(e2["min"], e2["max"])
    base_abs = key == "armor"
    f = (lambda lo, hi: t.rel(key, lo, hi)) if base_abs else (lambda lo, hi: t.absolute(key, max(0.0, lo), hi))
    s1 = cast(t, [e1], settle=1.5)
    f(lo1, hi1)
    cast(t, [e2], settle=2.0)
    f(lo1 + lo2, hi1 + hi2)                              # two spells: the magnitudes add
    t.wait(d1 + 3)                                       # the first has ended
    f(lo2, hi2)
    t.wait(16)
    f(0.0, 0.0)


# ---- effects that do nothing on the wrong kind of target (a scamp is a creature)

def gen_wrong_target(t, d, rng):
    eid = rng.choice([51, 53])           # Frenzy / Demoralize Humanoid: a creature takes neither
    kind = "refcombat" if eid == 51 else "refflee"
    begin(t, scamp=True)
    t.snap(kind, "scamp")
    cast(t, [fit(d, E(eid, 100, 100, 10, 2))], at="scamp", settle=3.0)
    t.rel(f"{kind}:scamp", 0, 0)


GROUPS = [
    ("drain / fortify attribute", lambda t, d, r: gen_drain_fortify(t, d, r, ATTR_EFFECTS), 1.0),
    ("drain / fortify skill", lambda t, d, r: gen_drain_fortify(t, d, r, SKILL_EFFECTS), 1.0),
    ("damage then restore attribute", lambda t, d, r: gen_damage_restore(t, d, r, ATTR_EFFECTS), 0.7),
    ("damage then restore skill", lambda t, d, r: gen_damage_restore(t, d, r, SKILL_EFFECTS), 0.7),
    ("damage per second attribute", lambda t, d, r: gen_damage_persecond(t, d, r, ATTR_EFFECTS), 0.4),
    ("damage per second skill", lambda t, d, r: gen_damage_persecond(t, d, r, SKILL_EFFECTS), 0.4),
    ("stacking attribute", lambda t, d, r: gen_stack(t, d, r, ATTR_EFFECTS), 0.4),
    ("stacking skill", lambda t, d, r: gen_stack(t, d, r, SKILL_EFFECTS), 0.4),
    ("drain with fortify attribute", lambda t, d, r: gen_drain_and_fortify_net(t, d, r, ATTR_EFFECTS), 0.4),
    ("drain with fortify skill", lambda t, d, r: gen_drain_and_fortify_net(t, d, r, SKILL_EFFECTS), 0.4),
    ("health", gen_health, 1.5),
    ("magicka", gen_magicka, 1.0),
    ("resist / weakness", gen_resist, 1.5),
    ("scamp elemental resist", scamp_elem, 0.5),
    ("absorb", gen_absorb, 0.7),
    ("cure / dispel", gen_cure_dispel, 0.5),
    ("modifier-only effects", gen_modifier, 1.0),
    ("wrong target", gen_wrong_target, 0.3),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20261002)
    ap.add_argument("--count", type=int, default=6, help="cases per group (scaled per group)")
    args = ap.parse_args()
    d = sg.Data()
    rng = random.Random(args.seed)
    t = ETest()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step(f"SAVE:{SLOT}", 1)
    for name, fn, scale in GROUPS:
        for _ in range(max(1, round(args.count * scale))):
            fn(t, d, rng)
    n = t.save("openmw-spec-effects.txt")
    print(f"openmw-spec-effects: {n} steps")


if __name__ == "__main__":
    main()
