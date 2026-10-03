"""World-rule spec tests (spec/world-rules.md): leveled lists, calendar, disposition, soul trap, followers,
journal, respawn timing, stacking. The formulas are OpenMW's, written out here in our own words; the data (leveled
lists, creature soul values, factions, journals, GMSTs) is what the engine loaded (out/world), so a failure is the
engine's logic and not the data.

  python tools/specgen_world.py [--seed N] [--count N]
writes
  tools/tests/openmw-spec-world.txt         only EXPECT kinds and setup tokens that exist today
  tools/tests/openmw-spec-world-hooks.txt   needs engine hooks that do not exist yet (listed in
                                            spec/findings/world-rules.md, "Hooks needed"); until they are
                                            added the unknown kinds are skipped or fail as "unknown kind"

Nothing here has been run against the engine (no emulator, no build); see the findings file.
"""
import argparse
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import specdata  # noqa: E402
from openmw_specgen import Data, Test, rel, load_actors, setup_stats, new_stats_test, load, PERSUADE_NPCS  # noqa: E402


# ------------------------------------------------------------------ leveled lists (levelledlist.cpp getLevelledItem)

def lev_candidates(lst, level):
    """The entries (positions in lst['e']) a roll can land on once the chance-none roll has passed."""
    e = lst["e"]
    highest = 0
    for lvl, _ in e:
        if highest < lvl <= level:
            highest = lvl
    allv = bool(lst.get("all"))
    return [i for i, (lvl, _) in enumerate(e) if level >= lvl and (allv or lvl == highest)]


def lev_pick(lst, level, roll_none, idx):
    """Position of the entry chosen with the given chance-none roll (0..99) and candidate index, -1 for nothing."""
    if roll_none < lst.get("none", 0):
        return -1
    c = lev_candidates(lst, level)
    if not c:
        return -1
    return c[idx % len(c)]


def gen_leveled(rng, count):
    g = specdata.load("game.json")
    t = Test()
    lists = {}
    lists.update({k: v for k, v in g.get("leveled_items", {}).items()})
    lists.update({k: v for k, v in g.get("leveled", {}).items() if k not in lists})
    # ('+' splits a harness token, ':' and ',' split the check's arguments)
    names = sorted(n for n in lists if not any(c in n for c in "+:,%"))
    for _ in range(count):
        name = rng.choice(names)
        lst = lists[name]
        level = rng.choice([1, 1, 2, 3, 5, 8, 12, 20, 30, 40, rng.randint(1, 60)])
        arg = name.replace(" ", "%")
        t.expect(f"levcandidates:{arg},{level}", *[len(lev_candidates(lst, level))] * 2)
        n = len(lev_candidates(lst, level))
        for roll in (0, max(0, lst.get("none", 0) - 1), lst.get("none", 0), 99):
            idx = rng.randint(0, 6)
            p = lev_pick(lst, level, roll, idx)
            t.expect(f"levpick:{arg},{level},{roll},{idx}", p, p)
    # the Each flag: a count over 1 on a top-level entry rolls once per item
    for name in names:
        lst = lists[name]
        if lst.get("each"):
            for c in (1, 2, 5):
                t.expect(f"levrolls:{name.replace(' ', '%')},{c}", c if c > 1 else 1, c if c > 1 else 1)
            break
    return t


# ------------------------------------------------------------------ calendar (datetimemanager.cpp, duration.hpp)

DAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
MAX_HOUR = 23.99999809265137          # the largest float below 24


class Cal:
    def __init__(self, hour, day, month, year, passed):
        self.hour, self.day, self.month, self.year, self.passed = hour, day, month, year, passed

    def set_hour(self, hour):
        hour = max(0.0, hour)
        days = int(hour / 24)
        self.hour = min(math.fmod(hour, 24), MAX_HOUR)
        if days > 0:
            self.set_day(days + self.day)       # (dayspassed is not touched by a set)

    def set_day(self, day):
        day = max(1, day)
        month = self.month
        while True:
            n = DAYS[month]
            if day <= n:
                break
            if month < 11:
                month += 1
            else:
                month = 0
                self.year += 1
            day -= n
        self.day, self.month = day, month

    def set_month(self, month):
        month = max(0, month)
        years, month = divmod(month, 12)
        if self.day > DAYS[month]:
            self.day = DAYS[month]
        self.month = month
        self.year += years

    def advance(self, hours):
        total = hours + self.hour
        self.set_hour(total)
        d = int(total / 24)
        if d > 0:
            self.passed += d

    def expect(self, t, with_hour=True):
        t.expect("global:day", self.day, self.day)
        t.expect("global:month", self.month, self.month)
        t.expect("global:year", self.year, self.year)
        t.expect("global:dayspassed", self.passed, self.passed)
        if with_hour:
            t.expect("hour", self.hour - 0.02, self.hour + 0.02)


def gen_calendar(rng, count):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specworld", 1)
    for k in range(count):
        load(t, "specworld")
        year = rng.randint(400, 430)
        month = rng.choice([0, 1, 2, 5, 6, 10, 11, rng.randint(0, 11)])
        day = rng.choice([1, DAYS[month], DAYS[month] - 1, rng.randint(1, DAYS[month])])
        passed = rng.randint(1, 900)
        hour = rng.choice([0.5, 6.5, 12.5, 18.5, 22.5, 23.5, rng.randint(0, 23) + 0.5])
        # (year, month, then day: setting the month would clamp a day that is not in it)
        t.step(f"SETGLOBAL:year:{year}", 0.1)
        t.step(f"SETGLOBAL:month:{month}", 0.1)
        t.step(f"SETGLOBAL:day:{day}", 0.1)
        t.step(f"SETGLOBAL:dayspassed:{passed}", 0.1)
        t.step(f"HOUR:{hour}", 0.1)
        cal = Cal(hour, day, month, year, passed)
        cal.expect(t)
        hrs = rng.choice([1, 2, 5, 8, 12, 24, 30, 48, 49, rng.randint(1, 72)])
        t.step(f"ADVANCE:{hrs}", 0.3)
        cal.advance(hrs)
        cal.expect(t)
    # the set rules (OpenMW wraps a day past the month's end, clamps a month's day, carries years)
    for day, month in ((32, 0), (60, 0), (29, 1), (0, 3), (-4, 3), (31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30 + 31 + 5, 0)):
        load(t, "specworld")
        t.step("SETGLOBAL:year:427", 0.1)
        t.step(f"SETGLOBAL:month:{month}", 0.1)
        t.step(f"SETGLOBAL:day:{day}", 0.1)
        cal = Cal(0, 1, month, 427, 1)
        cal.set_day(day)
        t.expect("global:day", cal.day, cal.day)
        t.expect("global:month", cal.month, cal.month)
        t.expect("global:year", cal.year, cal.year)
    for mon, day in ((13, 15), (12, 31), (25, 3), (1, 31), (3, 31), (5, 31)):
        load(t, "specworld")
        t.step("SETGLOBAL:year:427", 0.1)
        t.step("SETGLOBAL:month:0", 0.1)
        t.step(f"SETGLOBAL:day:{day}", 0.1)
        t.step(f"SETGLOBAL:month:{mon}", 0.1)
        cal = Cal(0, 1, 0, 427, 1)
        cal.day = day
        cal.set_month(mon)
        t.expect("global:day", cal.day, cal.day)
        t.expect("global:month", cal.month, cal.month)
        t.expect("global:year", cal.year, cal.year)
    # an hour past 24 set directly moves the calendar day, never the days passed
    for h in (24.5, 30.0, 49.25):
        load(t, "specworld")
        t.step("SETGLOBAL:year:427", 0.1)
        t.step("SETGLOBAL:month:11", 0.1)
        t.step("SETGLOBAL:day:30", 0.1)
        t.step("SETGLOBAL:dayspassed:100", 0.1)
        t.step(f"SETGLOBAL:gamehour:{h}", 0.1)
        cal = Cal(0, 30, 11, 427, 100)
        cal.set_hour(h)
        cal.expect(t)
    return t


def gen_timescale(rng, count):
    """Game hours per real second = timescale / 3600. A wall-clock wait, so the bound is loose (25 %)."""
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specworld", 1)
    for k in range(count):
        load(t, "specworld")
        ts = rng.choice([30, 60, 120, 360, 1000])
        secs = 6
        t.step("HOUR:8", 0.1)
        t.step(f"SETGLOBAL:timescale:{ts}", 0.1)
        t.step("SNAP:hour", 0.1)
        t.wait(secs)
        d = secs * ts / 3600.0
        t.step(f"EXPECT:hour:ge:@{d * 0.75 - 0.02:.4f}", 0.1)
        t.step(f"EXPECT:hour:le:@{d * 1.25 + 0.05:.4f}", 0.1)
    return t


# ------------------------------------------------------------------ disposition (mechanicsmanagerimp getDerivedDisposition)

def reaction(factions, a, b):
    return factions.get(a, {}).get("reactions", {}).get(b, 0)


def faction_term(d, factions, npc_faction, joined):
    """(rank, reaction) the formula uses; joined: {faction: rank} (none expelled)."""
    if not joined:
        return 0, 0
    if npc_faction in joined:
        return joined[npc_faction], reaction(factions, npc_faction, npc_faction)
    if not npc_faction:
        return 0, 0
    best = None
    for f, r in joined.items():               # (the first lowest reaction wins; tests keep it unique)
        re = reaction(factions, npc_faction, f)
        if best is None or re < best[1]:
            best = (r, re)
    return best


def gen_disposition(d, rng, count):
    """Deltas against an NPC with no faction ties, so the unknown race term (fDispRaceMod when races match)
    cancels. Personality and base are even numbers: the sum stays a whole number and nothing truncates."""
    actors = load_actors()
    factions = specdata.load("game_factions.json")["factions"]
    g = d.f
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specdisp", 1)
    pool = [n for n in PERSUADE_NPCS if n[0] in actors]
    others = [f for f in ("mages guild", "temple", "imperial legion", "redoran", "fighters guild", "thieves guild") if f in factions]
    for k in range(count):
        npc, cell = rng.choice(pool)
        a = actors[npc]
        nf = (a.get("faction") or "").lower()
        t.step("LOAD:specdisp", 0.5)
        t.wait(3)
        t.step(f"GOTO:{cell}", 0.3)
        t.wait(8)
        base = rng.choice([30, 36, 40, 44, 50, 56, 60])
        p0, p1 = 2 * rng.randint(15, 40), 2 * rng.randint(10, 48)
        t.step(f"SETATTR:personality:{p0}", 0.1)
        t.step(f"SETDISP:{npc}:{base}", 0.1)
        t.step(f"SNAP:refdisp:{npc}", 0.1)
        t.step(f"SETATTR:personality:{p1}", 0.1)
        dx = g("fdisppersonalitymult", 0.5) * (p1 - p0)
        # a faction setup: none, the NPC's own, or one or two others
        joined = {}
        mode = rng.choice(["none", "own", "other", "two"]) if nf else rng.choice(["none", "other", "two"])
        if mode == "own":
            joined[nf] = 2 * rng.randint(0, 2)
        elif mode == "other":
            joined[rng.choice([f for f in others if f != nf])] = 2 * rng.randint(0, 2)
        elif mode == "two":
            pair = [f for f in others if f != nf]
            f1, f2 = rng.sample(pair, 2)
            if reaction(factions, nf, f1) != reaction(factions, nf, f2) or not nf:
                if nf:
                    r = rng.choice([0, 2])
                    joined[f1], joined[f2] = r, r              # (equal ranks: no tie on which counts)
                else:
                    joined[f1] = 0                             # no faction: the rank and reaction are 0 either way
        for f, r in joined.items():
            t.step(f"JOIN:{f.replace(' ', '_')}:{r}", 0.1)
        rank, re = faction_term(d, factions, nf, joined)
        dx += (g("fdispfactionrankmult", 0.5) * rank + g("fdispfactionrankbase", 1.0)) * g("fdispfactionmod", 3.0) * re
        # the baseline had no faction term: nothing to take off
        bounty = rng.choice([0, 0, 40, 500])
        if bounty:
            t.step(f"SETBOUNTY:{bounty}", 0.1)
            dx -= g("fdispcrimemod", 0.0) * bounty
        lo0 = base + g("fdisppersonalitymult", 0.5) * (p0 - 50)         # (+ 0 or fDispRaceMod: the player's race)
        hi0 = lo0 + g("fdispracemod", 5.0)
        if min(lo0, lo0 + dx) < 2 or max(hi0, hi0 + dx) > 98:
            continue                                            # keep clear of the 0..100 clamp
        t.step(f"EXPECT:refdisp:{npc}:ge:@{math.floor(dx - 0.01)}", 0.1)
        t.step(f"EXPECT:refdisp:{npc}:le:@{math.ceil(dx + 0.01)}", 0.1)
    return t


# ------------------------------------------------------------------ soul trap (actors.cpp soulTrap)

GEMS = [("misc_soulgem_petty", 10), ("misc_soulgem_lesser", 20), ("misc_soulgem_common", 40),
        ("misc_soulgem_greater", 60), ("misc_soulgem_grand", 200), ("misc_soulgem_azura", 5000)]


def gen_soul(d, rng, count):
    mult = d.f("fsoulgemmult", 3.0)
    creatures = []
    for i in range(10):
        try:
            for a in specdata.load(f"game_actors_{i}.json")["actors"]:
                s = a.get("soul")
                if isinstance(s, int) and s > 0 and all(c.isalnum() or c in " _-" for c in a["id"]):
                    creatures.append((a["id"].lower(), s))
        except FileNotFoundError:
            break
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specsoul", 1)
    for k in range(count):
        cid, soul = rng.choice(creatures)
        arg = cid.replace(" ", "%")
        chosen = [(g, v, rng.randint(1, 3)) for g, v in GEMS if rng.random() < 0.6] or [GEMS[3] + (1,)]
        t.step("LOAD:specsoul", 0.5)
        t.wait(3)
        t.step("GOTO:Balmora,_Guild_of_Mages", 0.3)
        t.wait(6)
        for g, v, n in chosen:
            t.step(f"GIVE:{g}:{n}", 0.2)
        t.step("SPELL:soul_trap", 0.2)
        t.step(f"PLACE:{arg}:250", 0.3)
        t.step(f"SOULKILL:{arg}", 0.5)
        fit = [(v * mult, g) for g, v, n in chosen if v * mult >= soul]
        pick = min(fit)[1] if fit else None
        for g, v, n in chosen:
            t.step(f"EXPECT:count:{g}:eq:{n}", 0.1)            # a stack keeps its size: one gem is filled
            if g == pick:
                t.step(f"EXPECT:soul:{g}:{arg}", 0.1)
                if n > 1:
                    t.step(f"EXPECT:soul:{g}:none", 0.1)
            else:
                t.step(f"EXPECT:soul:{g}:none", 0.1)
    return t


# ------------------------------------------------------------------ followers (actionteleport.cpp getFollowers)

def follows(dist, stay_outside, to_exterior, in_exterior, hostile_to_player=False):
    if hostile_to_player:
        return False                        # included, then its combat with the player is stopped and it stays
    if not to_exterior and stay_outside and in_exterior:
        return False
    return dist * dist <= 800 * 800


def gen_followers(rng, count):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specfollow", 1)
    for k in range(count):
        # well inside or outside 800: the follower closes in while the player turns to the door (the edge itself is
        # the followset hook's, in the hooks test)
        dist = rng.choice([100, 300, 1500, 2500])
        npc = rng.choice(["guar", "scamp", "mudcrab", "kwama%forager"])
        t.step("LOAD:specfollow", 0.5)
        t.wait(3)
        t.step("GOTO:Seyda_Neen", 0.3)
        t.wait(8)
        t.step("TP:-11056:-72286:200:180", 1)           # at the Census and Excise Office door, facing the street
        t.wait(2)
        t.step(f"PLACE:{npc}:{dist}", 0.3)
        t.step(f"FOLLOW:{npc}", 0.3)
        t.step("DOORTO:Seyda_Neen,_Census_and_Excise_Office", 60)
        want = 1 if follows(dist, False, False, True) else 0
        t.step(f"EXPECT:refcell:{npc},Seyda%Neen,%Census:eq:{want}", 0.1)
    return t


# ------------------------------------------------------------------ journal (journalimp.cpp, quest.cpp)

def gen_journal_basic(rng, count):
    """Only the index: JOURNAL sets a quest's stage (the sweeps' setup); what a quest reports is its index."""
    j = {}
    for i in range(2):
        j.update(specdata.load(f"game_journal_{i}.json")["journal"])
    names = sorted(q for q, v in j.items() if len(v["entries"]) >= 3)
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specjournal", 1)
    for k in range(count):
        q = rng.choice(names)
        idxs = sorted({e["index"] for e in j[q]["entries"]})
        load(t, "specjournal")
        t.step(f"EXPECT:journal:{q}:eq:0", 0.1)
        i = rng.choice(idxs)
        t.step(f"JOURNAL:{q}:{i}", 0.1)
        t.step(f"EXPECT:journal:{q}:eq:{i}", 0.1)
    return t


def gen_journal_script(rng, count):
    """The script's Journal command and SetJournalIndex (hooks JOURNALADD / SETJOURNALINDEX).
    Journal: an index already heard only raises the stage; a new one raises the stage when higher, never lowers it,
    and writes a line when its text is not empty. Finished / restart flags come from the entry (none in our data yet,
    see the findings)."""
    j = {}
    for i in range(2):
        j.update(specdata.load(f"game_journal_{i}.json")["journal"])
    names = sorted(q for q, v in j.items() if len(v["entries"]) >= 3)
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specjournal", 1)
    for k in range(count):
        q = rng.choice(names)
        ents = {e["index"]: e for e in j[q]["entries"]}
        idxs = sorted(ents)
        load(t, "specjournal")
        index, heard, fin, lines = 0, set(), False, 0
        for _ in range(rng.randint(3, 7)):
            if rng.random() < 0.2:
                i = rng.randint(0, idxs[-1] + 10)
                t.step(f"SETJOURNALINDEX:{q}:{i}", 0.1)
                index = i                                   # set outright, even down; no line, no flags
            else:
                i = rng.choice(idxs)
                t.step(f"JOURNALADD:{q}:{i}", 0.1)
                e = ents[i]
                if i in heard:
                    if index < i:
                        index = i
                else:
                    if e["finished"]:
                        fin = True
                    if e["restart"]:
                        fin = False
                    if i > index:
                        index = i
                    if e["text"]:
                        heard.add(i)
                        lines += 1
            t.step(f"EXPECT:journal:{q}:eq:{index}", 0.1)
            t.step(f"EXPECT:journalentries:{q}:eq:{lines}", 0.1)
            t.step(f"EXPECT:questfinished:{q}:eq:{1 if fin else 0}", 0.1)
    return t


# ------------------------------------------------------------------ respawn, stacks, enable (hooks)

def gen_respawn_numbers(d):
    t = Test()
    t.step("CLASS:battlemage")
    months = d.f("imonthstorespawn", 4)
    t.expect("respawninterval", 24 * 30 * months, 24 * 30 * months)         # hours between container respawns
    t.expect("corpsedelay", d.f("fcorpsecleardelay", 72.0), d.f("fcorpsecleardelay", 72.0))
    t.expect("goldresetdelay", d.f("fbartergoldresetdelay", 24.0), d.f("fbartergoldresetdelay", 24.0))
    clear = d.f("fcorpsecleardelay", 72.0)
    # a corpse is cleared when the cell next loads and time of death + delay <= now (equal counts)
    for hours_after, gone in ((clear - 1, 0), (clear, 1), (clear + 5, 1)):
        t.step("GOTO:Seyda_Neen", 0.3)
        t.wait(6)
        t.step("PLACE:mudcrab:200", 0.3)
        t.step("SOULKILL:mudcrab", 0.5)
        t.step(f"ADVANCE:{hours_after}", 0.3)
        t.step("GOTO:Seyda_Neen,_Census_and_Excise_Office", 0.3)
        t.wait(3)
        t.step("GOTO:Seyda_Neen", 0.3)
        t.wait(6)
        t.expect("refexists:mudcrab", 1 - gone, 1 - gone)
    return t


def gen_stacks(rng):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("SAVE:specstack", 1)
    for k in range(6):
        load(t, "specstack")
        gem = "misc_soulgem_grand"
        n = rng.randint(2, 4)
        t.step(f"GIVE:{gem}:{n}", 0.2)
        t.expect(f"stackcount:{gem}", 1, 1)                  # same id, both empty: one stack
        t.step("SPELL:soul_trap", 0.2)
        t.step("GOTO:Balmora,_Guild_of_Mages", 0.3)
        t.wait(6)
        t.step("PLACE:scamp:250", 0.3)
        t.step("SOULKILL:scamp", 0.5)
        t.expect(f"stackcount:{gem}", 2, 2)                  # one gem split off with the soul in it
        t.expect(f"count:{gem}", n, n)
        t.step("PLACE:scamp:250", 0.3)
        t.step("SOULKILL:scamp", 0.5)
        t.expect(f"stackcount:{gem}", 2 if n > 2 else 1, 2 if n > 2 else 1)   # a second scamp soul restacks with the first
        t.expect(f"count:{gem}", n, n)
    # gold: adding a gold_100 by script is one gold_001; there is only one gold stack
    load(t, "specstack")
    t.step("SNAP:gold", 0.1)
    t.step("GIVE:gold_100:3", 0.2)
    t.step("EXPECT:gold:eq:@3", 0.1)
    t.step("GIVE:gold_001:7", 0.2)
    t.step("EXPECT:gold:eq:@10", 0.1)
    t.expect("stackcount:gold_001", 1, 1)
    return t


def gen_enable(rng):
    t = Test()
    t.step("CLASS:battlemage")
    t.step("GOTO:Seyda_Neen,_Census_and_Excise_Office", 0.3)
    t.wait(6)
    t.step("PLACE:guar:200", 0.3)
    t.expect("refenabled:guar", 1, 1)
    t.step("DISABLE:guar", 0.2)
    t.expect("refenabled:guar", 0, 0)
    t.step("ENABLE:guar", 0.2)
    t.expect("refenabled:guar", 1, 1)
    t.step("ENABLE:guar", 0.2)                               # enabling twice changes nothing
    t.expect("refenabled:guar", 1, 1)
    return t


def gen_soulcap(d):
    t = Test()
    t.step("CLASS:battlemage")
    for g, v in GEMS:
        c = v * d.f("fsoulgemmult", 3.0)
        t.expect(f"soulcapacity:{g}", c, c)
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20261002)
    ap.add_argument("--count", type=int, default=24)
    args = ap.parse_args()
    d = Data()
    rng = random.Random(args.seed)
    n = args.count

    main_t = Test()
    for part in (gen_calendar(rng, max(8, n // 2)), gen_timescale(rng, 4), gen_disposition(d, rng, 12),
                 gen_soul(d, rng, 10), gen_followers(rng, 8), gen_journal_basic(rng, 10)):
        # each part starts with its own header and setup lines; keep the first test's, drop the duplicate heads
        body = [ln for ln in part.lines[2:]]
        main_t.lines.extend(body)
    print("openmw-spec-world:", main_t.save("openmw-spec-world.txt"), "steps")

    hooks = Test()
    for part in (gen_leveled(rng, n * 2), gen_journal_script(rng, 10), gen_respawn_numbers(d), gen_stacks(rng),
                 gen_enable(rng), gen_soulcap(d)):
        hooks.lines.extend(part.lines[2:])
    print("openmw-spec-world-hooks:", hooks.save("openmw-spec-world-hooks.txt"), "steps")


if __name__ == "__main__":
    main()
