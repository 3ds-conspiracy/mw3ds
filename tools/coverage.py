"""What Morrowind.esm uses that the engine doesn't handle yet, ranked by how often it is used.

  python tools/coverage.py                     the whole game
  python tools/coverage.py --quest main        the main quest (journals A1_, A2_, B1_.. B8_, C0_.. C3_, CX_)
  python tools/coverage.py --quest "^hh_"      any journal-id regex (House Hlaalu here)

Counted: script functions (object scripts and dialogue result scripts), dialogue filter functions,
magic effects of spells / enchantments / potions / ingredients, NPC and creature AI packages.
"Supported" is read from the engine source (source/script.cpp's function table, dialogue.cpp's
filterFunction, magic.cpp's effect enum), so the report follows the code as it grows.
"""
import argparse
import collections
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import mwscript
from mwfiles import DATA_FILES, read_records

ROOT = Path(__file__).resolve().parents[1]
MAIN_QUEST = r"^(a1|a2|b\d|c\d|cx)_"

# Dialogue filter function codes (SCVR type '1'), as the construction set numbers them
FILTER_NAMES = {
    0: "rank low", 1: "rank high", 2: "rank requirement", 3: "reputation", 4: "health percent",
    5: "pc reputation", 6: "pc level", 7: "pc health percent", 8: "pc magicka", 9: "pc fatigue",
    10: "pc strength", **{11 + k: f"pc skill {k}" for k in range(27)}, 38: "pc sex", 39: "pc expelled",
    40: "pc common disease", 41: "pc blight disease", 42: "pc clothing modifier", 43: "pc crime level",
    44: "same sex", 45: "same race", 46: "same faction", 47: "faction rank difference", 48: "detected",
    49: "alarmed", 50: "choice", **{51 + k: f"pc attribute {k + 1}" for k in range(7)}, 58: "pc corprus",
    59: "weather", 60: "pc vampire", 61: "level", 62: "attacked", 63: "talked to pc", 64: "pc health",
    65: "creature target", 66: "friend hit", 67: "fight", 68: "hello", 69: "alarm", 70: "flee",
    71: "should attack", 72: "werewolf", 73: "werewolf kills",
}
# Magic effect indices (MGEF INDX) 0..136
MAGIC_NAMES = [
    "WaterBreathing", "SwiftSwim", "WaterWalking", "Shield", "FireShield", "LightningShield", "FrostShield", "Burden",
    "Feather", "Jump", "Levitate", "SlowFall", "Lock", "Open", "FireDamage", "ShockDamage",
    "FrostDamage", "DrainAttribute", "DrainHealth", "DrainMagicka", "DrainFatigue", "DrainSkill", "DamageAttribute", "DamageHealth",
    "DamageMagicka", "DamageFatigue", "DamageSkill", "Poison", "WeaknessToFire", "WeaknessToFrost", "WeaknessToShock", "WeaknessToMagicka",
    "WeaknessToCommonDisease", "WeaknessToBlightDisease", "WeaknessToCorprusDisease", "WeaknessToPoison", "WeaknessToNormalWeapons", "DisintegrateWeapon", "DisintegrateArmor", "Invisibility",
    "Chameleon", "Light", "Sanctuary", "NightEye", "Charm", "Paralyze", "Silence", "Blind",
    "Sound", "CalmHumanoid", "CalmCreature", "FrenzyHumanoid", "FrenzyCreature", "DemoralizeHumanoid", "DemoralizeCreature", "RallyHumanoid",
    "RallyCreature", "Dispel", "Soultrap", "Telekinesis", "Mark", "Recall", "DivineIntervention", "AlmsiviIntervention",
    "DetectAnimal", "DetectEnchantment", "DetectKey", "SpellAbsorption", "Reflect", "CureCommonDisease", "CureBlightDisease", "CureCorprusDisease",
    "CurePoison", "CureParalyzation", "RestoreAttribute", "RestoreHealth", "RestoreMagicka", "RestoreFatigue", "RestoreSkill", "FortifyAttribute",
    "FortifyHealth", "FortifyMagicka", "FortifyFatigue", "FortifySkill", "FortifyMaximumMagicka", "AbsorbAttribute", "AbsorbHealth", "AbsorbMagicka",
    "AbsorbFatigue", "AbsorbSkill", "ResistFire", "ResistFrost", "ResistShock", "ResistMagicka", "ResistCommonDisease", "ResistBlightDisease",
    "ResistCorprusDisease", "ResistPoison", "ResistNormalWeapons", "ResistParalysis", "RemoveCurse", "TurnUndead", "SummonScamp", "SummonClannfear",
    "SummonDaedroth", "SummonDremora", "SummonAncestralGhost", "SummonSkeletalMinion", "SummonBonewalker", "SummonGreaterBonewalker", "SummonBonelord", "SummonWingedTwilight",
    "SummonHunger", "SummonGoldenSaint", "SummonFlameAtronach", "SummonFrostAtronach", "SummonStormAtronach", "FortifyAttack", "CommandCreature", "CommandHumanoid",
    "BoundDagger", "BoundLongsword", "BoundMace", "BoundBattleAxe", "BoundSpear", "BoundLongbow", "ExtraSpell", "BoundCuirass",
    "BoundHelm", "BoundBoots", "BoundShield", "BoundGloves", "Corprus", "Vampirism", "SummonCenturionSphere", "SunDamage",
    "StuntedMagicka",
]


# Functions the engine accepts but that do nothing yet (keep in step with script.cpp's no-op cases)
# Accepted by the engine but without effect (rotating baked objects, weather, UI history, voices)
STUBS = {"rotate", "modregion", "dontsaveobject", "changeweather", "clearinfoactor", "raiserank",
         "getcurrentweather", "equip", "streammusic", "sethello"}


def engine_script_functions():
    src = (ROOT / "source" / "script.cpp").read_text()
    names = set(re.findall(r'X\(\w+, "(\w+)"\)', src))
    # statFunction: Get / Set / Mod + attribute or skill, Get / Set / Mod / ModCurrent + health etc.
    for table in ("kAttrFuncs", "kSkillFuncs"):
        m = re.search(table + r"\[\d+\] = \{(.*?)\};", src, re.S)
        for stat in re.findall(r'"(\w+)"', m.group(1)) if m else []:
            names |= {"get" + stat, "set" + stat, "mod" + stat}
    for stat in ("health", "magicka", "fatigue"):
        names |= {"get" + stat, "set" + stat, "mod" + stat, "modcurrent" + stat}
    return names


def engine_filter_functions():
    src = (ROOT / "source" / "dialogue.cpp").read_text()
    body = src[src.index("static float filterFunction"):]
    body = body[:body.index("\n}\n")]
    handled = set(int(n) for n in re.findall(r"case (\d+):", body))
    handled |= set(int(n) for n in re.findall(r"f == (\d+)", body))
    for a, b in re.findall(r"f >= (\d+) && f <= (\d+)", body):
        handled |= set(range(int(a), int(b) + 1))
    return handled


def engine_effects():
    src = (ROOT / "source" / "magic.cpp").read_text()
    enum = src[src.index("EFF_"):]
    enum = enum[:enum.index("};")]
    out = {int(v): n for n, v in re.findall(r"EFF_(\w+) = (\d+)", enum)}
    # summons and bound items are handled by index ranges (summonCreature / boundItem)
    if "summonCreature" in src:
        out.update({e: "summon" for e in list(range(102, 117)) + [134]})
    if "boundItem" in src:
        out.update({e: "bound" for e in range(120, 132) if e != 126})
    return out


def calls(node, out):
    if isinstance(node, list):
        if len(node) >= 4 and node[0] == "c" and isinstance(node[2], str):
            out.add(node[2])
        for x in node:
            calls(x, out)
    elif isinstance(node, (dict, tuple)):
        for x in (node.values() if isinstance(node, dict) else node):
            calls(x, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quest", help='journal-id regex, or "main" for the main quest')
    ap.add_argument("--top", type=int, default=40, help="rows per section")
    args = ap.parse_args()
    quest = re.compile(MAIN_QUEST if args.quest == "main" else args.quest, re.I) if args.quest else None

    globs, scripts, dials, effect_names = set(), [], [], {}
    ai = collections.Counter()
    effects_used = collections.Counter()
    cur = None
    for r in read_records(DATA_FILES / "Morrowind.esm"):
        t = r.tag
        if t == "GLOB":
            globs.add((r.id or "").lower())
        elif t == "SCPT":
            text = (r.get("SCTX") or b"").decode("latin-1")
            name = (r.get("SCHD") or b"")[:32].split(b"\0", 1)[0].decode("latin-1")
            scripts.append((name, text))
        elif t == "DIAL":
            cur = {"id": (r.id or "").lower(), "type": (r.get("DATA") or b"\0")[0], "infos": []}
            dials.append(cur)
        elif t == "INFO" and cur is not None:
            cur["infos"].append(r)
        elif t in ("SPEL", "ENCH", "ALCH"):
            for tt, dd in r.subs:
                if tt == "ENAM":
                    effects_used[struct.unpack_from("<h", dd)[0]] += 1
        elif t == "INGR":
            d = r.get("IRDT")
            for e in struct.unpack_from("<4i", d, 8):
                if e >= 0:
                    effects_used[e] += 1
        elif t in ("NPC_", "CREA"):
            for tt in ("AI_W", "AI_T", "AI_F", "AI_E", "AI_A"):
                if r.get(tt) is not None:
                    ai[tt] += 1

    def about_quest(text):
        if not quest:
            return True
        return any(quest.search(q) for q in re.findall(r'journal[,\s"]+([\w\']+)', text, re.I)) \
            or any(quest.search(q) for q in re.findall(r'getjournalindex[,\s"(]+([\w\']+)', text, re.I))

    # ---- script functions
    uses = collections.Counter()
    where = collections.defaultdict(set)
    n_scripts = n_results = failed = 0
    for name, text in scripts:
        if not about_quest(text):
            continue
        n_scripts += 1
        found = set()
        try:
            calls(mwscript.compile_script(text, globs), found)
        except Exception:
            failed += 1
        for f in found:
            uses[f] += 1
            where[f].add(name)
    filters = collections.Counter()
    n_infos = 0
    for dial in dials:
        for info in dial["infos"]:
            conds = [d.decode("latin-1") for tt, d in info.subs if tt == "SCVR"]
            text = (info.get("BNAM") or b"").decode("latin-1")
            journal_ids = [c[5:] for c in conds if len(c) > 5 and c[1] == "4"]
            if quest and not (quest.search(dial["id"]) or any(quest.search(j) for j in journal_ids)
                              or about_quest(text)):
                continue
            n_infos += 1
            for c in conds:
                if len(c) > 4 and c[1] == "1" and c[2:4].isdigit():
                    filters[int(c[2:4])] += 1
            if text.strip():
                n_results += 1
                found = set()
                try:
                    calls(mwscript.compile_snippet(text, globs), found)
                except Exception:
                    failed += 1
                for f in found:
                    uses[f] += 1
                    where[f].add("dialogue " + dial["id"])

    have = engine_script_functions()
    missing = sorted(((n, f) for f, n in uses.items() if f not in have), reverse=True)
    stubbed = sorted(((n, f) for f, n in uses.items() if f in have and f in STUBS), reverse=True)
    print(f"Morrowind.esm{' / quest ' + quest.pattern if quest else ''}: {n_scripts} scripts, "
          f"{n_infos} dialogue responses ({n_results} with result scripts), {failed} that didn't compile")
    print(f"\nScript functions: {len(uses)} used, {len(uses) - len(missing)} supported, {len(missing)} missing "
          f"({sum(n for n, _ in missing)} of {sum(uses.values())} uses)")
    for n, f in missing[:args.top]:
        sample = sorted(where[f])[:3]
        print(f"  {n:5d}  {f:28s} e.g. {', '.join(sample)}")
    if stubbed:
        print(f"  accepted but without effect yet ({sum(n for n, _ in stubbed)} uses): "
              + ", ".join(f"{f} {n}" for n, f in stubbed))

    handled = engine_filter_functions()
    fmiss = sorted(((n, f) for f, n in filters.items() if f not in handled), reverse=True)
    print(f"\nDialogue filter functions: {len(filters)} used, {len(filters) - len(fmiss)} supported "
          f"({sum(n for n, _ in fmiss)} of {sum(filters.values())} conditions unsupported)")
    for n, f in fmiss[:args.top]:
        print(f"  {n:5d}  {f:3d} {FILTER_NAMES.get(f, '?')}")

    if not quest:
        eff = engine_effects()
        emiss = sorted(((n, e) for e, n in effects_used.items() if e not in eff), reverse=True)
        print(f"\nMagic effects: {len(effects_used)} used, {len(effects_used) - len(emiss)} in the engine, "
              f"{len(emiss)} missing ({sum(n for n, _ in emiss)} of {sum(effects_used.values())} uses)")
        print("  " + ", ".join(f"{MAGIC_NAMES[e] if 0 <= e < len(MAGIC_NAMES) else e} {n}" for n, e in emiss[:args.top]))
        print(f"\nAI packages on NPCs / creatures: " + ", ".join(
            f"{ {'AI_W': 'wander', 'AI_T': 'travel', 'AI_F': 'follow', 'AI_E': 'escort', 'AI_A': 'activate'}[k]} {v}"
            for k, v in ai.most_common()))


if __name__ == "__main__":
    main()
