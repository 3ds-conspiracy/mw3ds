"""Script functions: OpenMW's against ours, and which the master files use.

Reads OpenMW's function and instruction names from its compiler (build/openmw-full/components/compiler/
extensions0.cpp, plus the ones it registers in loops: attributes, skills, health / magicka / fatigue, magic
effects, the player-control switches), ours from source/components/interpreter/scriptfunctions.hpp (the function table and the stat names statFunction takes
families), and every script and dialogue result in Morrowind.esm, Tribunal.esm and Bloodmoon.esm.

Usage:
    python tools/test/script_gap.py              # missing functions, grouped by OpenMW file and by who uses them
    python tools/test/script_gap.py --missing    # only the missing names, one per line
"""
import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "convert"))

ATTRS = "strength intelligence willpower agility speed endurance personality luck".split()
DYN = "health magicka fatigue".split()
SKILLS = ("block armorer mediumarmor heavyarmor bluntweapon longblade axe spear athletics enchant destruction "
          "alteration illusion conjuration mysticism restoration alchemy unarmored security sneak acrobatics "
          "lightarmor shortblade marksman mercantile speechcraft handtohand").split()
EFFECTS = ("resistmagicka resistfire resistfrost resistshock resistdisease resistblight resistcorprus resistpoison "
           "resistparalysis resistnormalweapons waterbreathing chameleon waterwalking swimspeed superjump flying "
           "armorbonus castpenalty silence blindness paralysis invisible attackbonus defendbonus").split()
CONTROLS = "playercontrols playerfighting playerjumping playerlooking playermagic playerviewswitch vanitymode".split()
MASTERS = ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm")


def openmw_names():
    """{OpenMW extension group: set of names}"""
    src = (ROOT / "build/openmw-full/components/compiler/extensions0.cpp").read_text()
    groups = collections.OrderedDict()
    for m in re.finditer(r"namespace (\w+)\s*\{(.*?)\n    \}", src, re.S):
        groups[m.group(1)] = set(re.findall(r'register(?:Instruction|Function)\(\s*"(\w+)"', m.group(2)))
    stats = groups.setdefault("Stats", set())
    for a in ATTRS + SKILLS:
        stats |= {"get" + a, "set" + a, "mod" + a}
    for d in DYN:
        stats |= {"get" + d, "set" + d, "mod" + d, "modcurrent" + d, "get" + d + "getratio"}
    for e in EFFECTS:
        stats |= {"get" + e, "set" + e, "mod" + e}
    control = groups.setdefault("Control", set())
    for c in CONTROLS:
        control |= {"enable" + c, "disable" + c, "get" + c + "disabled"}
    groups.setdefault("Gui", set()).add("messagebox")
    groups.setdefault("Transformation", set()).update({"getpos", "setpos", "getposition"})
    return {k: v for k, v in groups.items() if v}


def our_names():
    src = (ROOT / "source/components/interpreter/scriptfunctions.hpp").read_text(encoding="utf-8", errors="replace")
    names = set(re.findall(r'X\(\w+, "(\w+)"\)', src))
    for a in ATTRS + SKILLS:
        names |= {"get" + a, "set" + a, "mod" + a}
    for d in DYN:
        names |= {"get" + d, "set" + d, "mod" + d, "modcurrent" + d, "get" + d + "getratio"}
    names |= {"getlevel", "setlevel", "modpccrimelevel"}
    return names


def usage(all_names):
    """{master file: Counter of names its scripts and dialogue results call}"""
    import mwfiles
    out = {}
    for esm in MASTERS:
        path = mwfiles.DATA_FILES / esm
        use = collections.Counter()
        if path.exists():
            for r in mwfiles.read_records(path, {"SCPT", "INFO"}):
                text = r.get("SCTX") if r.tag == "SCPT" else r.get("BNAM")
                for line in (text or b"").decode("latin-1").split("\n"):
                    line = re.sub(r'"[^"]*"?', "", line).split(";")[0]      # not inside strings or comments
                    for w in set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", line.lower())):
                        if w in all_names:
                            use[w] += 1
        out[esm] = use
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--missing", action="store_true", help="only the missing names")
    args = ap.parse_args()
    groups = openmw_names()
    ours = our_names()
    all_names = set().union(*groups.values())
    missing = sorted(all_names - ours)
    if args.missing:
        print("\n".join(missing))
        return
    use = usage(all_names)
    print(f"OpenMW {len(all_names)}, ours {len(all_names & ours)}, missing {len(missing)}")
    for g, names in groups.items():
        miss = sorted(names - ours)
        print(f"  {g}: {len(names)}, missing {len(miss)}")
    for esm in MASTERS:
        used = [n for n in missing if use[esm][n]]
        print(f"\nmissing, used by {esm} ({len(used)}):")
        print("  " + (", ".join(f"{n} ({use[esm][n]})" for n in used) or "none"))
    unused = [n for n in missing if not any(use[e][n] for e in MASTERS)]
    print(f"\nmissing, used by no master file ({len(unused)}):")
    print("  " + ", ".join(unused))
    extra = sorted(ours - all_names)
    if extra:
        print(f"\nours, not OpenMW's: {', '.join(extra)}")


if __name__ == "__main__":
    main()
