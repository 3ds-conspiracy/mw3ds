"""Writes tools/test/cases/shrines-saints.txt: every saint's shrine (the scripts that ask for a donation, then offer
Cure Disease, Cure Blight, Cure Poison and the saint's own blessing: shrineRilm, shrineVivecFury ...) asked for its
blessing: the donation taken (what its script asks of someone not in its faith: 35, the Imperial Cult's 25), the blessing's cast seen and heard (OpenMW's Cast by an
object: the cast on the shrine, the hit on the player) and its lasting effect on the player.

  python3.13 tools/test/specgen_shrines.py
  python3.13 tools/test/run_test.py shrines-saints --native --data out/world --start 'Seyda Neen'

The Temple's pilgrimage shrines (shrineKummu, shrineStopMoon ...) are played in uber-tribunal-temple.
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "convert"))
import mwfiles  # noqa: E402

SCHOOLS = ["alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration"]
# (effects whose hit leaves nothing to read later: instant ones, and those that take the player away)
INSTANT = {57, 60, 61, 62, 63, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78}


def main():
    master = mwfiles.load_order()[0]
    scripts = {}
    for r in mwfiles.read_records(master, {"SCPT"}):
        t = r.zstr("SCTX") or ""
        name = t.split("\n", 1)[0].replace("Begin", "").strip().lower()
        m = re.search(r'(?i)"What blessing do you ask for\?"\s+"Cure Disease"\s+"Cure Blight"\s+"Cure Poison"\s+"([^"]+)"', t)
        cast = re.findall(r'(?i)if \( button == 3 \)\s*\n\s*Cast\s+"([^"]+)"\s+Player', t)
        price = re.search(r'(?i)\belse\b[ \t]*(?:;[^\n]*)?\n\s*Set donation to (\d+)', t)
        if m and cast:
            scripts[name] = (cast[0], int(price.group(1)) if price else 35)
    acti = {}
    for r in mwfiles.read_records(master, {"ACTI"}):
        sc = (r.zstr("SCRI") or "").lower()
        if sc in scripts:
            acti[(r.zstr("NAME") or "").lower()] = sc
    spells, mgef = {}, {}
    for r in mwfiles.read_records(master, {"SPEL", "MGEF"}):
        if r.tag == "SPEL":
            spells[(r.zstr("NAME") or "").lower()] = [struct.unpack_from("<hbbiiiii", d) for t, d in r.subs if t == "ENAM"]
        else:
            mgef[struct.unpack("<i", dict(r.subs)["INDX"])[0]] = struct.unpack_from("<i", dict(r.subs)["MEDT"])[0]
    where = {}
    for r in mwfiles.read_game({"CELL"}):
        for ref in mwfiles.cell_refs(r):
            rid = (ref["id"] or "").lower()
            if rid in acti and acti[rid] not in where and r.zstr("NAME"):
                where[acti[rid]] = (r.zstr("NAME"), rid)
    lines = ["# start: Seyda Neen",
             "# shrines-saints: written by tools/test/specgen_shrines.py (edit the generator, not this file).",
             "# Every saint's shrine asked for its own blessing: the donation taken, the blessing's hit heard, its lasting effect on.",
             "3 0 0 0 0 MSG", "0.3 0 0 0 0 SEED:1+GOD:2", "0.3 0 0 0 0 GIVE:Gold_001:1000"]
    checks = 0
    for sc, (spell, price) in sorted(scripts.items()):
        if sc not in where:
            lines.append(f"# {sc}: no shrine placed")
            continue
        cell, rid = where[sc]
        effs = spells.get(spell.lower(), [])
        school = SCHOOLS[mgef.get(effs[0][0], 5)] if effs else "restoration"
        lines.append(f"# {sc} ({spell}) in {cell}")
        lines += [f"0.3 0 0 0 0 GOTO:{cell.replace(' ', '_')}", "4 0 0 0",
                  f"0.1 0 0 0 0 SNAP:gold+SNAP:soundstarted:{school}_hit",
                  f"60 0 0 0 0 ACTIVATE:{rid.replace(' ', '%')}", "1 0 0 0", "0.3 0 0 0 0 A", "1 0 0 0",
                  "0.2 0 0 0 0 DOWN", "0.2 0 0 0 0 DOWN", "0.2 0 0 0 0 DOWN", "0.3 0 0 0 0 A", "1.3 0 0 0",
                  f"0.1 0 0 0 0 EXPECT:gold:eq:@-{price}", f"0.1 0 0 0 0 EXPECT:soundstarted:{school}_hit:gt:@0"]
        checks += 2
        lasting = [e for e in effs if e[0] not in INSTANT and e[5] > 2]
        if lasting:
            e = lasting[0]
            lines.append(f"0.1 0 0 0 0 EXPECT:effect:{e[0]}:ge:{e[6]}" if e[0] not in (79, 83, 80, 81, 82)
                         else f"0.1 0 0 0 0 EXPECT:activespells:ge:1")
            checks += 1
        lines += ["0.3 0 0 0 0 B", "0.5 0 0 0 0 B", "1 0 0 0"]
    out = HERE / "cases" / "shrines-saints.txt"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"shrines-saints: {len(scripts)} shrines, {checks} checks")


if __name__ == "__main__":
    main()
