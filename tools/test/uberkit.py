"""Writes the first pass of an uber quest case for the v2 test kit: tools/test/cases/v1/<name>.txt (the v1 case, kept out of
the suites' folder) -> tools/test/cases/<name>.txt. The v2 case is hand-maintained once it passes.

The kit (one ENHANCEV2 step in the setup block, see internal/uber-v2-handoff.md): a cast-when-used ring that fortifies
Strength, Speed, the six schools and the magicka pool for the whole run, and a fly item (Levitate for one second a use)
that the v2 FLYTO uses again and again: no potions, no casts, no waiting for a levitation to run out. The rewrite:
  A  ENHANCEV2 after the BOOST / ACTIVE lines of the setup block; the levitation, speed and magicka potion GIVEs go
  B  no potions in v2 (their GIVEs go too): a levitation run a FLYTO or a driver walk follows -> nothing (the FLYTO flies
     on the item, the ring runs faster than v1 flew); before a flight by hand (jumps, R / L) -> FLOAT:60; speed / magicka -> nothing
     (speed-only runs go); a run before anything else (a flight by hand, timed for the potions' speed) stays
  B2 a DOORTO / ACTIVATE right after a FLYTO -> FLOAT:60 from there (v1 went on on the potions' leftover levitation)
  C  where v1 waited the levitation out (a wait of 20 s or more, CAST:dispel, EXPECT:effect:10:eq:0) after a flight or
     a float -> FLOAT:off if on, then UNTIL:effect:10:eq:0 (the item's last second); a second one dropped A wait whose comment says it is for something
     else (a movie, a duel, a farewell) is kept
  D  "N 0 0 0" followed by EXPECT:cell:X -> UNTIL:cell:X (10 s at most) and a 0.5 s settle
  E  SAVE:uber-xx-<point> / LOAD: -> <case name>-<point> (the v1 checkpoints stay as they are)
  F  a move by hand (stick, jump, R / L / UP / DOWN, no driver verb) -> a third of its time (the ring's Speed: ~3x)
  H  a wait of 1.5 s or less after TOPIC / CHOICE -> UNTIL:talkready; after B -> UNTIL:screen:none
  G  per case (HINTS): lines before / after a given action, or the line replaced (a flight where the v1 walk got through by luck, the fly spell where
     v1 walked on with the potions' leftover levitation)

  python tools/test/uberkit.py <name> [--out <path>] [--quiet]      (prints every change unless --quiet)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASES = ROOT / "tools" / "test" / "cases"
POTIONS = ("USE:p_levitation", "USE:p_fortify_speed_e", "USE:p_restore_magicka", "USE:p_chameleon", "USE:p_invisibility")   # (prefixes)
POTION_GIVES = ("GIVE:p_levitation", "GIVE:p_fortify_speed_e", "GIVE:p_restore_magicka", "GIVE:p_chameleon", "GIVE:p_invisibility")
FLOAT_ON, FLOAT_OFF = "0.1 0 0 0 0 FLOAT:60", "0.1 0 0 0 0 FLOAT:off"   # (60: the potions' seconds)
# cases whose floats go on through doors (FLOAT:60:doors): Telvanni towers, whose floors only levitation joins, as v1's
# potions carried on into them
FLOAT_DOORS = ("uber-house-telvanni",)
LAND = ["5 0 0 0 0 UNTIL:effect:10:eq:0"]   # (the item's last second running out: ends the frame it does)
KEEP_WAIT = ("movie", "duel", "farewell", "plays by itself", "holds the player", "wait it out", "catch up", "walks")
SPEED_RATIO = 1 / 3      # v1 run speed over the kit's (550 / 1650): moves by hand are cut to this share of their time
# G: flights the rules can not see, by case: before a step with this exact action, fly to the spot first (fly, FLYTO,
# land). For walks v1 only got through by luck that the kit's speed changes (Wolverine Hall to Gals Arethi: off the
# platform's edge, landing either side of a ledge)
HINTS = {
    "uber-house-redoran": {
        # (Odral Helvi's chest: the book stolen in his sight brings a guard, and the fine takes the book; v1 was unseen by
        # timing. Sneaking, once nobody is looking)
        "USELOCKPICK:com_chest_02_helvi:pick_grandmaster": (["0.3 0 0 0 0 SNEAK:1", "70 0 0 0 0 UNSEEN:60"], []),
        "LOOT:com_chest_02_helvi:bk_calderarecordbook2": ([], ["0.3 0 0 0 0 SNEAK:0"]),
    },
    "uber-imperial-cult": {
        # (back from the nix-hound in Rotheran's arena: by the vault door, which the walk unlocks, not the grid's way round
        # through the seats and the arena's fighters)
        "WALKTO:@1052,220,-704": (["60 0 0 0 0 WALKTO:@547,766,-704"], []),
    },
    "uber-tribunal-temple": {
        # (Wolverine Hall to Gals Arethi: v1 walked off the platform's edge and landed on the right side of a ledge by luck)
        "ACTIVATE:gals%arethi": (["120 0 0 0 0 FLYTO:@142120,39366,258:beside", "LAND"], []),
        # (the flooded St. Delyn Underworks: v1 walked them hovering on the potions' leftover levitation; down once in)
        "DOORTO:Ihinipalit,_Shrine": (["0.1 0 0 0 0 FLOAT:60"], ["0.1 0 0 0 0 FLOAT:off", "LAND"]),
    },
    "uber-morag-tong": {
        # (the flooded St. Olms Underworks: v1 walked them hovering on the potions' leftover levitation; the float ends at the
        # shrine's door by itself)
        "DOORTO:Assernerairan,_Shrine": (["0.1 0 0 0 0 FLOAT:60"], []),
    },
    "uber-main-quest": {
        # (Progress of Truth, taken sneaking only: v1 was unseen by the wandering ordinator's timing. Hidden)
        "PICKUP:bk_progressoftruth": (["1 0 0 0 0 HIDE:20"], ["0.1 0 0 0 0 HIDE:off"]),
        # (Holamayan: the ground at the spot is 2311, not 2073; v1 stopped 238 over it, which its 250 took for the ground, and
        # the kit's 200 does not: the way down beside it boxed the flight in under the rock cover)
        "FLYTO:@159100,-30521,2073": ([], [], "120 0 0 0 0 FLYTO:@159100,-30521,2311"),
        # (the Cavern of the Incarnate's door opens 6..8: v1 slept to 5 and took the hour to get there; the kit is there in minutes)
        "SLEEPUNTIL:5": ([], [], "0.3 0 0 0 0 SLEEPUNTIL:6"),
    },
}
WINDOW = 8             # a wait this many steps after a fly cast (or fewer) is taken for the levitation being waited out


class Line:
    def __init__(self, text):
        self.text = text
        s = text.strip()
        self.comment = s.startswith("#")
        self.blank = not s
        self.fields = s.split() if not (self.comment or self.blank) else []
        self.step = bool(self.fields)
        self.secs = float(self.fields[0]) if self.step else 0.0
        self.verb = self.fields[5] if len(self.fields) >= 6 and not re.match(r"^-?[0-9.]+$", self.fields[5]) else ""
        self.plain_wait = self.step and not self.verb and len(self.fields) == 4
        self.out = [text]          # what this line becomes (several lines, or none)
        self.note = ""


def convert(lines, name, log):
    float_on = FLOAT_ON + (":doors" if name in FLOAT_DOORS else "")
    L = [Line(t) for t in lines]
    steps = [i for i, l in enumerate(L) if l.step]
    changes = {"A": 0, "B": 0, "C": 0, "D": 0, "E": 0, "F": 0, "G": 0, "H": 0}
    saved = 0.0

    # A: the kit after the setup's BOOST and the ACTIVE lines that follow it
    boost = next((i for i in steps if L[i].verb.startswith("BOOST:")), None)
    if boost is None:
        sys.exit(f"{name}: no BOOST step: not an uber case?")
    at = boost
    k = steps.index(boost)
    while k + 1 < len(steps) and L[steps[k + 1]].verb.startswith("ACTIVE:"):
        k += 1
        at = steps[k]
    L[at].out.append("0.3 0 0 0 0 ENHANCEV2")
    changes["A"] += 1
    log(f"A  line {at + 1}: ENHANCEV2 after {L[at].verb}")

    # A, again: no potions in v2 (the ring does Speed and magicka, the fly item the levitation)
    for i in steps:
        if L[i].verb.startswith(POTION_GIVES):
            L[i].out = []
            log(f"A  line {i + 1}: dropped {L[i].verb}")

    # B: potion runs. Before a FLYTO: nothing (it flies on the item). A levitation run before anything else (a flight by
    # hand: jumps, R / L, a walk over water): FLOAT:on, the item used again every half second till C turns it off
    k = 0
    while k < len(steps):
        if L[steps[k]].verb.startswith(POTIONS):
            j = k
            while j + 1 < len(steps) and L[steps[j + 1]].verb.startswith(POTIONS):
                j += 1
            run = [steps[m] for m in range(k, j + 1)]
            lev = any(L[i].verb.startswith("USE:p_levitation") for i in run)
            nxt = L[steps[j + 1]].verb if j + 1 < len(steps) else ""
            for i in run:
                L[i].out = []
            saved += 0.3 * len(run)
            # (chameleon / invisibility potions: the kit's hide item, kept on 60 s as theirs, off before a talk)
            if any(L[i].verb.startswith(("USE:p_chameleon", "USE:p_invisibility")) for i in run):
                L[run[0]].out = ["1 0 0 0 0 HIDE:60"]   # (a second: one who saw us before it still counts as a witness for one)
            if lev and nxt.startswith("FLYTO:"):
                L[run[-1]].lev = "fly"
                what = "nothing (the FLYTO flies on the item)"
            elif lev and nxt.startswith(("ACTIVATE:", "DOORTO:", "KILL:", "LOOT:", "PICKUP:", "STRIKE:", "WALKTO:"))                     and not nxt.startswith("WALKTO:@"):
                # (levitation and speed for a faster walk to someone in v1: the ring's Speed runs faster still, and a float would
                # hold the walk off the path's floor: Elith-Pal Mine, the walk to Dangor. A WALKTO to a spot keeps its float:
                # that is up to a floor, a Telvanni pod's, that only levitation reaches)
                what = "nothing (a driver walk: the ring is faster)"
            elif lev:
                L[run[-1]].out = [float_on]
                L[run[-1]].lev = "float"
                what = "FLOAT:on (a flight by hand)"
            else:
                what = "nothing"
            changes["B"] += 1
            log(f"B  lines {run[0] + 1}-{run[-1] + 1}: {len(run)} potions -> {what}; next {nxt or '(end)'}")
            k = j + 1
        else:
            k += 1

    # C: where v1 waited the levitation out (a wait of 20 s or more, CAST:dispel, EXPECT:effect:10:eq:0) within 8 steps
    # after a flight or with a float on: FLOAT:off if on, and UNTIL:effect:10:eq:0 (the item's last second); a second
    # wait for the same levitation dropped. Other long waits stay (listed: C?)
    lev, since, done = "", -100, True
    for k, i in enumerate(steps):
        l = L[i]
        if getattr(l, "lev", ""):
            lev, since, done = l.lev, k, False
            continue
        ends = (l.plain_wait and l.secs >= 20) or l.verb == "CAST:dispel" or l.verb == "EXPECT:effect:10:eq:0"
        if not ends or l.out != [l.text]:
            continue
        if l.verb == "EXPECT:effect:10:eq:0":
            # (always down first: a float the walk started can still be on; ends the frame nothing levitates)
            l.out = [FLOAT_OFF] + list(LAND) + [l.text]
            changes["C"] += 1
            if lev:
                lev, done = "", True
            continue
        nxt = L[steps[k + 1]].verb if k + 1 < len(steps) else ""
        if l.plain_wait and nxt == "EXPECT:effect:10:eq:0" and not lev:
            # (v1 waited its potions out here; v2's levitation is down already)
            l.out = []
            saved += l.secs
            changes["C"] += 1
            log(f"C- line {i + 1}: {l.secs:.0f} s wait dropped (the levitation is down already)")
            continue
        near = k - since <= WINDOW
        c = next((L[m].text.lower() for m in range(i - 1, -1, -1) if L[m].comment or L[m].step), "")
        if not lev or not near or (l.plain_wait and c.startswith("#") and any(w in c for w in KEEP_WAIT)):
            if l.plain_wait:
                prev = next((L[steps[m]].verb for m in range(k - 1, -1, -1) if L[steps[m]].verb), "")
                log(f"C? line {i + 1}: {l.secs:.0f} s wait kept (after {prev[:40] or 'a wait'})")
            continue
        land = [FLOAT_OFF] + list(LAND)   # (off: a float still going from earlier would hold the levitation up)
        if l.verb == "EXPECT:effect:10:eq:0":
            l.out = land + [l.text]       # (always: ends the frame nothing levitates, so it costs nothing)
        elif l.verb == "CAST:dispel":
            # (kept: it ends a spell's levitation, a shrine's blessing, Stop the Moon's Levitate 100 for 1500 s)
            l.out = [l.text] + land
            done = False
        else:
            l.out = [] if done else land
            saved += l.secs if l.plain_wait else 0.3
        log(f"C{'-' if done else ' '} line {i + 1}: {l.verb or f'{l.secs:.0f} s wait'} -> {'dropped (down already)' if done else 'down'}")
        if lev == "float":
            lev = ""
        done = True
        changes["C"] += 1

    # B2: a door or a person straight after a flight (plain waits between skipped): v1 went on still levitating, the potions
    # outlasting the flight by most of their 60 s, through the door and up to a chest on a shelf or a door above its walkway
    # (Vivec, Redoran Waistworks; Pelagiad, Mebestien's chest). v2's item stops a second after landing: FLOAT:60 from there
    # on, as the potions; C turns it off where v1 waited it out
    for k, i in enumerate(steps[:-1]):
        if not getattr(L[i], "lev", "") == "fly":
            continue
        f = k + 1
        while f < len(steps) and not L[steps[f]].verb.startswith("FLYTO:"):
            f += 1
        if f >= len(steps):
            continue
        n = f + 1
        while n < len(steps) and L[steps[n]].plain_wait:
            n += 1
        if n < len(steps) and L[steps[n]].verb.startswith(("DOORTO:", "ACTIVATE:")) and L[steps[n]].out == [L[steps[n]].text]:
            L[steps[n]].out = [float_on, L[steps[n]].text]
            L[steps[n]].lev = "float"
            changes["B"] += 1
            log(f"B2 line {steps[n] + 1}: {L[steps[n]].verb} after a flight: FLOAT:60 from there, as v1's potions")

    # D: a wait before EXPECT:cell
    for k, i in enumerate(steps):
        l = L[i]
        if l.plain_wait and l.out == [l.text] and k + 1 < len(steps):
            e = L[steps[k + 1]]
            if e.verb.startswith("EXPECT:cell:") and e.out == [e.text]:
                l.out = [f"10 0 0 0 0 UNTIL:{e.verb[7:]}", "0.5 0 0 0"]
                e.out = []
                saved += l.secs - 0.5
                changes["D"] += 1
            # (and a wait of a second or more before a journal check: a scripted scene's entry comes when it comes, the
            # kit's quicker moves start it sooner or later than v1's: Azura's farewell, 3 s after the jump, past v1's 2 s)
            elif l.secs >= 1.0 and re.match(r"EXPECT:journal:[^:]+:ge:\d+$", e.verb) and e.out == [e.text]:
                l.out = [f"{max(10.0, 3 * l.secs):g} 0 0 0 0 UNTIL:{e.verb[7:]}"]
                saved += l.secs
                changes["D"] += 1

    # F: moves by hand (the stick, a jump, R / L / UP / DOWN with no driver verb), timed for v1's speed: the ring's Speed
    # makes running, swimming and potion flight about 3x faster (run 550 -> 1650), so a third of the time covers the
    # same ground (the Puzzle Canal's dive overshot the spot that raises the bridge)
    for i in steps:
        l = L[i]
        f = l.fields
        if not l.step or l.out != [l.text] or len(f) < 4:
            continue
        try:
            mx, my = float(f[1]), float(f[2])
        except ValueError:
            continue
        jump = len(f) >= 5 and f[4] == "1"
        keys = l.verb in ("R", "L", "UP", "DOWN")
        if (mx or my or jump or keys) and (not l.verb or keys):
            secs = max(0.1, round(l.secs * SPEED_RATIO, 2))
            l.out = [" ".join([f"{secs:g}"] + f[1:])]
            saved += l.secs - secs
            changes["F"] += 1
            log(f"F  line {i + 1}: {l.secs:g} s of moving by hand -> {secs:g} s")

    # H: the waits after talking: a short plain wait (1.5 s or less) right after TOPIC / CHOICE / CHOICEVAL / PERSUADE ->
    # UNTIL:talkready (the answer is up, or the talk closed by it); after B (the talk closed) -> UNTIL:screen:none.
    # Dialogue happens within the frame here, so these end at once; a check that never holds fails the step
    TALK = ("TOPIC:", "CHOICE:", "CHOICEVAL:", "PERSUADE:")
    for k, i in enumerate(steps):
        l = L[i]
        if not (l.plain_wait and l.secs <= 1.5 and l.out == [l.text]) or k == 0:
            continue
        prev = L[steps[k - 1]]
        if prev.verb.startswith(TALK):
            l.out = ["2 0 0 0 0 UNTIL:talkready"]
        elif prev.verb == "B":
            nxt = L[steps[k + 1]].verb if k + 1 < len(steps) else ""
            if nxt.startswith("ESCORT:") and nxt != "ESCORT:-":
                # (an escort starts with the next walk: their follow package starts only once they see us near, which the
                # wait gave them; the walk at once would leave them standing)
                l.out = [f"5 0 0 0 0 UNTIL:following:{nxt[7:]}"]
            else:
                l.out = ["2 0 0 0 0 UNTIL:screen:none"]
        else:
            continue
        saved += l.secs
        changes["H"] += 1

    # G: hinted flights
    for i in steps:
        l = L[i]
        hint = HINTS.get(name, {}).get(l.verb)
        if hint and l.out == [l.text]:
            lines_of = lambda hs: [x for h in hs for x in (list(LAND) if h == "LAND" else [h])]
            before, after = lines_of(hint[0]), lines_of(hint[1])
            # (a third part: the line itself replaced)
            l.out = before + [hint[2] if len(hint) > 2 else l.text] + after
            changes["G"] += 1
            log(f"G  line {i + 1}: {len(before)} lines before {l.verb}, {len(after)} after")

    # E: the checkpoints
    for i in steps:
        m = re.match(r"^(.*\s)(SAVE|LOAD):(\S+)$", L[i].text)
        if m and L[i].out == [L[i].text]:
            short = re.match(r"uber-[a-z]+-(.+)$", m.group(3))
            L[i].out = [f"{m.group(1)}{m.group(2)}:{name}-{short.group(1) if short else m.group(3)}"]
            changes["E"] += 1

    out = [f"# {name}: played LEGIT on the v2 test kit (ENHANCEV2: the ring, the fly item; FLYTO / FLOAT / UNTIL). First pass",
           f"# generated by tools/test/uberkit.py from v1/{name}.txt; hand-maintained once it passes: edit this file"]
    for l in L:
        out += l.out
    # (the hide item's Chameleon is 300; v1 checked its stacked potions, 200 and more: any hide passes ge 100)
    out = [re.sub(r"EXPECT:effect:(40|39):ge:\d+", r"EXPECT:effect:\1:ge:100", x) for x in out]
    return out, changes, saved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--out")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    src = CASES / "v1" / f"{a.name}.txt"
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    log = (lambda s: None) if a.quiet else (lambda s: print("  " + s))
    out, changes, saved = convert(lines, a.name, log)
    dst = Path(a.out) if a.out else CASES / f"{a.name}.txt"
    dst.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{dst}: {len(out)} lines; A kit {changes['A']}, B potion runs {changes['B']}, C landings {changes['C']}, "
          f"D until {changes['D']}, E saves {changes['E']}, F hand moves {changes['F']}, G hinted flights {changes['G']}, H talk waits {changes['H']}; fixed waits cut by {saved:.0f} s (the flights and walks come on top)")


if __name__ == "__main__":
    main()
