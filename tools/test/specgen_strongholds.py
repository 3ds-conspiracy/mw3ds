"""Writes the build-stage cases: the three Great House strongholds and Bloodmoon's Raven Rock, stage by stage.

  python3.13 tools/test/specgen_strongholds.py
  python3.13 tools/test/run_test.py spec-stronghold-hlaalu --native --data out/world --start 'Seyda Neen'
  python3.13 tools/test/run_test.py spec-colony-stages-1 --native --data out/tb --start 'Seyda Neen'

Nothing in the engine builds them: the game data does. Every piece of a stronghold (Odai Plateau for Hlaalu, Bal Isra
for Redoran, Uvirith's Grave and Tel Uvirith for Telvanni) runs one of Morrowind.esm's Strong* scripts, which enable or
disable it by the Stronghold global (0 nothing, 1 the first stage being built, 2 built, 3 and 4 the second stage, 5 and
6 the third); the builder (strong_foreman) stands there only while a stage is being built and moves on a cell change.
Raven Rock's buildings run Bloodmoon.esm's Colony_E_* and ColonyFactor* scripts on ColonyState (1 to 30 the colony's
four stages, 31 to 33 the Factor's Estate being built, 34 to 36 the estate the player chose). The rules below are those
scripts, read from the data files; each case sets the global as the quest would, leaves the place and comes back (the
scripts run when the cell is active, as OpenMW runs local scripts of disabled references too), then checks that each
script's pieces are all enabled or all disabled, and takes a shot.
"""
from pathlib import Path

CASES = Path(__file__).resolve().parent / "cases"


# Each script's rule: (stage, enabled before, its locals) -> enabled after. None: the script leaves it as it is
def toggle(on):
    return lambda s, e, loc: on(s)


STRONG = {
    "StrongZero": toggle(lambda s: s == 0),
    "Strong1": toggle(lambda s: s > 0),
    "Strong1_Construct": toggle(lambda s: s == 1),
    "Strong1_Complete": toggle(lambda s: s >= 2),
    "Strong1_Only": toggle(lambda s: s == 2),
    "Strong2": toggle(lambda s: s > 2),
    "Strong2_Construct": toggle(lambda s: s == 3),
    "Strong2_Complete": toggle(lambda s: s >= 4),
    "Strong2_Only": toggle(lambda s: s == 4),
    "Strong2_Not": toggle(lambda s: s < 3),
    "Strong3": toggle(lambda s: s > 4),
    "Strong3_Construct": toggle(lambda s: s == 5),
    "Strong3_Complete": toggle(lambda s: s >= 6),
    "Strong3_Not": toggle(lambda s: s < 5),
    "strong_foreman": toggle(lambda s: s in (1, 3, 5)),
    "strongholdServices": toggle(lambda s: s >= 2),      # the people living there
}


def staged(first, last):
    """Colony_E_<n>_M: on from `first` (once: a 'done' local), off again from `last`."""
    def rule(s, e, loc):
        if e and s < first:
            e = False
        if not loc.get("done") and not e and s >= first:
            e, loc["done"] = True, 1
        if e and s >= last:
            e = False
        return e
    return rule


def window(first, only=None):
    """ColonyFactor<n>_Mid / _Final: off below `first`, on at it, off above `only` when given."""
    def rule(s, e, loc):
        if e and s < first:
            e = False
        if not e and s == first:
            e = True
        if e and only is not None and s > only:
            e = False
        return e
    return rule


def dropped(*at):
    """ColonyFactor<n>_D: pieces of the old buildings, disabled for good at the given stages."""
    return lambda s, e, loc: e and not any(f(s) for f in at)


def torch(s, e, loc):
    if s < 30:
        return False
    if s == 30:
        return True
    return None if s >= 34 else e      # SetDelete 1 from 34: gone, neither enabled nor disabled


COLONY = {
    "Colony_E_1_M": staged(1, 3),
    "Colony_E_1_Mine": toggle(lambda s: s >= 2),
    "Colony_E_1_F": toggle(lambda s: s >= 3),
    "Colony_E_2_M": staged(3, 10),
    "Colony_E_2_F": toggle(lambda s: s >= 10),
    "Colony_E_3_M": staged(12, 20),
    "Colony_E_3_F": toggle(lambda s: s >= 20),
    "Colony_E_4_M": staged(22, 30),
    "Colony_E_4_F": toggle(lambda s: s >= 30),
    "Colony_E_Torch": torch,
    "ColonyFactor1_Mid": window(31, 31),
    "ColonyFactor2_Mid": window(32, 32),
    "ColonyFactor3_Mid": window(33, 33),
    "ColonyFactor1_Final": window(34, 34),
    "ColonyFactor2_Final": window(35, 35),
    "ColonyFactor3_Final": window(36),
    "ColonyFactor1_D": dropped(lambda s: s == 31, lambda s: s == 34),
    "ColonyFactor2_D": dropped(lambda s: s == 32, lambda s: s == 35),
    "ColonyFactor3_D": dropped(lambda s: s >= 33),
}

# Where each place is: the spot the checks run from (the builder's, then the way to face: the main building), a place far off to leave for, and
# which scripts have pieces there (Morrowind.esm / Bloodmoon.esm, by cell)
AWAY = "GOTO:Seyda_Neen"
SITES = {
    "hlaalu": ("TP:-35423:-36869:1960:0|FACE:@-35882,-36003", ["Strong1", "Strong1_Construct", "Strong1_Complete", "Strong2",
               "Strong2_Construct", "Strong2_Complete", "Strong3", "Strong3_Construct", "Strong3_Complete",
               "strong_foreman"]),
    "redoran": ("TP:-34865:78785:1920:0|FACE:@-36000,78464", ["Strong1", "Strong1_Construct", "Strong1_Complete", "Strong2",
                "Strong2_Construct", "Strong2_Complete", "Strong3", "Strong3_Construct", "Strong3_Complete",
                "strong_foreman"]),
    "telvanni": ("TP:85770:11352:1950:0|FACE:@87381,9474", ["Strong1_Construct", "Strong1_Only", "Strong2", "Strong2_Construct",
                 "Strong2_Complete", "Strong2_Only", "Strong3", "Strong3_Construct", "Strong3_Complete",
                 "strong_foreman"]),
}
INSIDE = {
    "hlaalu": ("GOTO:Rethan_Manor,_Gols'_House", ["strongholdServices"]),
    "redoran": ("GOTO:Indarys_Manor,_Manor_Services", ["strongholdServices"]),
    "telvanni": ("GOTO:Tel_Uvirith,_Tower_Lower", ["Strong3", "Strong3_Complete", "Strong3_Not"]),
}
ESTATE = ("GOTO:Raven_Rock,_Factor's_Estate", ["ColonyFactor1_Final", "ColonyFactor2_Final", "ColonyFactor3_Final"])
RAVEN_ROCK = ("TP:-200500:157800:900:0|FACE:@-201300,159600", "GOTO:Raven_Rock,_Bar")


def checks(rules, names, stage, state):
    """The step checking every script's pieces at this stage (rules run as the cell becomes active)."""
    out = []
    for name in names:
        loc = state.setdefault(name, {"on": True})
        # A script runs every frame: run its rule until it settles (a 'done' local can change it twice)
        for _ in range(3):
            if loc["on"] is None:
                break
            loc["on"] = rules[name](stage, loc["on"], loc)
        if loc["on"] is None:
            out.append(f"EXPECT:scriptenabled:{name}:eq:0")
        elif loc["on"]:
            out += [f"EXPECT:scriptenabled:{name}:gt:0", f"EXPECT:scriptdisabled:{name}:eq:0"]
        else:
            out += [f"EXPECT:scriptenabled:{name}:eq:0", f"EXPECT:scriptdisabled:{name}:gt:0"]
    return "+".join(out)


def steps(tokens):
    """Checks over several steps: a step's line has a length limit."""
    t = tokens.split("+")
    return ["0.1 0 0 0 0 " + "+".join(t[i:i + 8]) for i in range(0, len(t), 8)]


def case(name, head, global_, stages, spot, away, rules, names, extra=None):
    lines = head + ["3 0 0 0 0 MSG", "0.3 0 0 0 0 GOD"]
    state, extra_state = {}, {}
    for stage in stages:
        lines += [f"# {global_} {stage}",
                  f"0.3 0 0 0 0 SETGLOBAL:{global_}:{stage}+{away}", "4 0 0 0",
                  f"0.3 0 0 0 0 {spot.split('|')[0]}", "4 0 0 0"]
        if "|" in spot:
            lines += [f"0.2 0 0 0 0 {spot.split('|')[1]}", "1 0 0 0"]
        lines += steps(checks(rules, names, stage, state))
        lines[-1] += "+SHOT"
        if extra:
            ex_spot, ex_names = extra
            lines += [f"0.3 0 0 0 0 {ex_spot}", "4 0 0 0"] + steps(checks(rules, ex_names, stage, extra_state))
    (CASES / f"{name}.txt").write_text("\n".join(lines) + "\n", newline="\n")
    print("wrote", name)


def main():
    note = "# Generated by tools/test/specgen_strongholds.py: edit the generator, not this file"
    for house, (spot, names) in SITES.items():
        head = [note, f"# House {house.title()}'s stronghold, stage by stage (the Stronghold global, Morrowind.esm's Strong* scripts)"]
        case(f"spec-stronghold-{house}", head, "stronghold", range(0, 7), spot, AWAY, STRONG, names,
             INSIDE[house])
    # Raven Rock: the colony's stages, then each of the three ways the Factor's Estate is built
    names = list(COLONY)
    spot, away = RAVEN_ROCK
    for n, path in ((1, [31, 34]), (2, [32, 35]), (3, [33, 36])):
        head = [note, "# data: bloodmoon",
                f"# Raven Rock's buildings by ColonyState (Bloodmoon.esm's Colony_E_* and ColonyFactor* scripts), the Factor's Estate way {n}"]
        stages = [0, 1, 2, 3, 10, 12, 20, 22, 30] if n == 1 else [30]
        case(f"spec-colony-stages-{n}", head, "colonystate", stages + path, spot, away, COLONY, names, ESTATE)


if __name__ == "__main__":
    main()
