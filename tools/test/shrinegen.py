"""Writes the shrine cases, tools/test/cases/shrine-<script>-<n>.txt: every blessing of every donation shrine in
Morrowind.esm (the Imperial Cult's altars, the Tribunal's and Veloth's shrines, the saints' shrines in the temples), as a
player asks for it, from the game's own scripts.

    python tools/test/shrinegen.py [--dry-run]

Each case goes to one place the shrine stands, gives the player the trouble the blessing is for (a common disease, a
blight, poison, a damaged Strength), activates the shrine, answers its donation question Yes and picks the blessing by
moving down the list, then checks what the script says happens: the donation taken (a stranger's: the Imperial
Cult's 25, the Temple's 35), the disease or poison gone, the attribute restored, or the blessing's spell running (its
first effect, at least its magnitude). The Imperial Cult's Restore Attributes also checks magicka stays as it was:
its spells restore attributes and skills only (issue #63, an Atronach asking for magicka back). Cases are generated:
edit this script, not the .txt files.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "convert"))
from mwfiles import DATA_FILES, MASTER, read_records  # noqa: E402

CASES = ROOT / "tools" / "test" / "cases"
# The donation shrines: their script, and the faction whose rank sets the donation (a stranger's below)
DONATION = {"Imperial Cult": 25, "Temple": 35}
DISEASE, BLIGHT = "rust_chancre", "ash_woe_blight"
EFFECT_POISON = 27


def load():
    scripts, objects, places, spells = {}, {}, {}, {}
    for r in read_records(DATA_FILES / MASTER, {"SCPT", "ACTI", "STAT", "CONT", "SPEL", "CELL"}):
        if r.tag == "SCPT":
            scripts[r.get("SCHD")[:32].rstrip(b"\0").decode("latin-1").lower()] = r.get("SCTX").decode("latin-1")
        elif r.tag == "SPEL":
            spells[r.id.lower()] = [struct.unpack("<hbbiiiii", d) for t, d in r.subs if t == "ENAM"]
        elif r.tag in ("ACTI", "STAT", "CONT"):
            s = (r.zstr("SCRI") or "").lower()
            if s:
                objects.setdefault(s, []).append(r.id)
        elif r.tag == "CELL" and r.id:
            for t, d in r.subs:
                if t == "NAME":
                    places.setdefault(d.rstrip(b"\0").decode("latin-1").lower(), []).append(r.id)
    return scripts, objects, places, spells


def blessings(text, stranger):
    """(button, label, spell ids cast, gold taken from a stranger) of a donation shrine's second question, or None when
    it isn't one. The gold is what the button's own lines take for the stranger's donation: the Tribunal's shrines ask
    "donation == 25" for their last two (Vivec's Mystery, Almsivi Restoration), so a stranger's 35 is never taken
    there, as in Morrowind"""
    m = re.search(r'MessageBox\s+"What blessing do you ask for\?"((?:\s+"[^"]*")+)', text)
    if not m or "donation" not in text:
        return None
    labels = re.findall(r'"([^"]*)"', m.group(1))
    tail = text[text.index("QuestionState == 20"):]
    out = []
    for b, label in enumerate(labels):
        part = re.search(r"if \( button == %d \)(.*?)Set QuestionState to 0" % b, tail, re.S | re.I)
        casts = re.findall(r'Cast\s+"?([^"\r\n]+?)"?\s+Player', part.group(1), re.I) if part else []
        taken = 0
        for n, g in re.findall(r"donation == (\d+) \)\s*Player->RemoveItem Gold_001 (\d+)", part.group(1) if part else ""):
            if int(n) == stranger:
                taken = int(g)
        out.append((b, label, [c.strip().lower() for c in casts], taken))
    return out


def restores(casts, spells):
    """The blessing is a restoration (Restore Attributes, Almsivi Restoration): spells of Restore Attribute only."""
    return any(effects and all(e[0] == 74 for e in effects) for effects in (spells.get(c) for c in casts))


def case(script, obj, cell, faction, b, label, casts, taken, spells, atronach=False):
    donation = DONATION[faction]
    lines = [
        "# start: Seyda Neen",
        f"# Written by tools/test/shrinegen.py: {script} ({obj} in {cell}), the blessing \"{label}\" asked for as a stranger",
        f"# (a donation of {donation}, {taken} of it taken by the script); casts {', '.join(casts) or 'nothing'}",
        "3 0 0 0 0 MSG",
        "0.3 0 0 0 0 SEED:3",
        "0.3 0 0 0 0 GIVE:Gold_001:500",
    ]
    checks = []
    low = label.lower()
    if atronach:
        # Born under the Atronach (Wombburned: Spell Absorption 50): the shrine casts its blessing, so the player may
        # absorb it, each effect its own roll, and gets each absorbed spell's cost back as magicka (OpenMW's
        # absorbSpell; issue #63). Magicka set low first: it comes back over that
        lines[1] += " by an Atronach"
        lines += ["0.3 0 0 0 0 SPELL:wombburn", "0.5 0 0 0", "0.3 0 0 0 0 SETMAGICKA:10"]
        checks += ["EXPECT:magicka:gt:11"]
    elif low == "cure disease":
        lines += [f"0.3 0 0 0 0 SPELL:{DISEASE}", "0.3 0 0 0 0 EXPECT:diseases:eq:1"]
        checks += ["EXPECT:diseases:eq:0"]
    elif low == "cure blight":
        lines += [f"0.3 0 0 0 0 SPELL:{BLIGHT}", "0.3 0 0 0 0 EXPECT:diseases:eq:1"]
        checks += ["EXPECT:diseases:eq:0"]
    elif low == "cure poison":
        lines += [f"0.3 0 0 0 0 ACTIVE:{EFFECT_POISON}:1:600", f"0.3 0 0 0 0 EXPECT:effect:{EFFECT_POISON}:gt:0"]
        checks += [f"EXPECT:effect:{EFFECT_POISON}:eq:0"]
    elif restores(casts, spells):
        # (a restoration: Strength damaged first, by a spell cast on self)
        lines += ["0.3 0 0 0 0 SETSKILL:destruction:100", "0.3 0 0 0 0 ADDEFFECT:22:20:20:1:0:strength",
                  "0.3 0 0 0 0 MAKESPELL:hurtstr", "1 0 0 0 0 CAST:mw3ds_test_hurtstr", "3 0 0 0",
                  "0.3 0 0 0 0 EXPECT:attrdamage:strength:ge:19"]
        checks += ["EXPECT:attrdamage:strength:lt:1"]
        if faction == "Imperial Cult":
            # (issue #63: no magicka comes back; Restore Attributes and the skill spells restore nothing else)
            lines += ["0.3 0 0 0 0 SETMAGICKA:10"]
            checks += ["EXPECT:magicka:le:11"]
    else:
        for c in casts:
            effects = spells.get(c)
            if effects:
                e = effects[0]
                checks.append(f"EXPECT:effect:{e[0]}:ge:{max(1, e[6])}")
    lines += [f"0.3 0 0 0 0 GOTO:{cell.replace(' ', '_')}", "4 0 0 0", "0.3 0 0 0 0 SNAP:gold",
              f"60 0 0 0 0 ACTIVATE:{obj}", "1 0 0 0 0 EXPECT:message:receive_a_blessing", "0.4 0 0 0 0 A",
              "1 0 0 0 0 EXPECT:message:What_blessing_do_you_ask_for"]
    lines += ["0.3 0 0 0 0 DOWN"] * b
    lines += ["0.4 0 0 0 0 A", "2 0 0 0", f"0.3 0 0 0 0 EXPECT:gold:eq:@-{taken}" if taken else "0.3 0 0 0 0 EXPECT:gold:eq:@0"]
    lines += [f"0.3 0 0 0 0 {c}" for c in checks]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    scripts, objects, places, spells = load()
    written = []
    for name, text in sorted(scripts.items()):
        if not name.startswith("shrine"):
            continue
        faction = "Imperial Cult" if '"Imperial Cult"' in text else "Temple"
        bl = blessings(text, DONATION[faction])
        objs = objects.get(name, [])
        cells = [c for o in objs for c in places.get(o.lower(), [])]
        if not bl or not cells:
            continue
        obj = next(o for o in objs if places.get(o.lower()))
        cell = sorted(places[obj.lower()])[0]
        for b, label, casts, taken in bl:
            path = CASES / f"shrine-{name[6:]}-{b}.txt"
            written.append(path.name)
            if not a.dry_run:
                path.write_text(case(name, obj, cell, faction, b, label, casts, taken, spells), encoding="utf-8", newline="\n")
            if restores(casts, spells):
                # (and asked for by an Atronach: the blessing absorbed, magicka back)
                path = CASES / f"shrine-{name[6:]}-{b}-atronach.txt"
                written.append(path.name)
                if not a.dry_run:
                    path.write_text(case(name, obj, cell, faction, b, label, casts, taken, spells, True), encoding="utf-8",
                                    newline="\n")
    print(f"shrinegen: {len(written)} cases" + (" (dry run)" if a.dry_run else ""))
    for w in written:
        print("  " + w)


if __name__ == "__main__":
    main()
