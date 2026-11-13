"""Checks a PC save (.ess) against the game files it names as masters, for what makes the PC game crash or lose
things on loading it: what ess_export.py writes must name what Morrowind.esm, Tribunal.esm and Bloodmoon.esm hold.

    python tools/test/ess_check.py <save.ess> [more ...]

For each save:
  - the header's cell (GMDT, and the GAME record's) is a cell of the masters, by the name the PC game knows it by;
  - every reference in a CELL record names, by its FRMR (master, number), a reference of that master in a cell of
    the same name, of the same object;
  - a global script's SCHD (its locals' counts and sizes) is the one of the last master that has the script, in the
    save's master order (the order the PC game loaded them in), and its SLCS gives those counts;
  - actors', creatures' and containers' records and their items, souls, the player's spells and known topics, the
    globals and the journal's topics name records of the masters (or of the save itself: a made spell or potion).
Prints what doesn't hold; exits 1 when anything doesn't.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "convert"))
import mwtools  # noqa: F401  (.env settings)
import mwfiles
from mwfiles import read_records, text

SKIP = {"INFO", "TES3", "LAND", "PGRD", "LTEX", "SKIL", "MGEF"}
# made by the PC game itself: the player's own record, a global the game adds (GOTY's monthly restock counter)
ENGINE_IDS = {"playersavegame", "monthstorespawn"}


def subs(body):
    i = 0
    while i < len(body):
        t = body[i:i + 4].decode("latin-1")
        (n,) = struct.unpack_from("<I", body, i + 4)
        yield t, body[i + 8:i + 8 + n]
        i += 8 + n


def records(path):
    b = Path(path).read_bytes()
    i = 0
    while i < len(b):
        tag = b[i:i + 4].decode("latin-1")
        (n,) = struct.unpack_from("<I", b, i + 4)
        yield tag, list(subs(b[i + 16:i + 16 + n]))
        i += 16 + n


def zlow(v):
    return text(v.split(b"\0", 1)[0]).lower()


class Masters:
    """What the save's masters define: ids by type, cells by name, references by (master, number), scripts' SCHD."""

    def __init__(self, names):
        self.ids, self.cells, self.refs, self.schd = {}, set(), {}, {}
        for k, name in enumerate(names, 1):
            for r in read_records(mwfiles.game_path(mwfiles.DATA_FILES, name)):
                if r.tag == "SCPT":
                    h = r.get("SCHD")
                    sid = text(h[:32]).lower()
                    self.schd[sid] = (name, h[32:52])
                    self.ids.setdefault(sid, set()).add("SCPT")
                elif r.tag == "CELL":
                    cname = (r.id or "").lower()
                    self.cells.add(cname)
                    num = None
                    for t, v in r.subs[1:]:
                        if t == "FRMR":
                            num = struct.unpack("<I", v)[0] & 0xFFFFFF
                        elif t == "NAME" and num is not None:
                            self.refs[(k, num)] = (cname, text(v).lower())
                            num = None
                elif r.tag not in SKIP and r.id:
                    self.ids.setdefault(r.id.lower(), set()).add(r.tag)
                    if r.tag == "REGN":
                        # (the header names a wilderness cell by its region, as the PC game shows it)
                        self.cells.add((r.zstr("FNAM") or r.id).lower())


def check(path):
    recs = list(records(path))
    head = dict(recs[0][1])
    names = [text(v) for t, v in recs[0][1] if t == "MAST"]
    m = Masters(names)
    own = {zlow(dict(s)["NAME"]) for t, s in recs if t in ("SPEL", "ALCH", "ENCH", "CLAS") and "NAME" in dict(s)}
    problems = []

    def need(what, rid, tags=None):
        if not rid or rid in ENGINE_IDS or rid in own:
            return
        if rid not in m.ids or (tags and not m.ids[rid] & tags):
            problems.append(f"{what} {rid!r} is not in the masters")

    gmdt = head.get("GMDT")
    if gmdt and zlow(gmdt[24:88]) not in m.cells:
        problems.append(f"header cell {zlow(gmdt[24:88])!r} is not a cell of the masters")
    refs = 0
    for tag, ss in recs[1:]:
        d = dict(ss)
        if tag == "GAME" and zlow(d["GMDT"][:64]) not in m.cells:
            problems.append(f"GAME cell {zlow(d['GMDT'][:64])!r} is not a cell of the masters")
        elif tag == "CELL":
            cname = zlow(ss[0][1])
            if cname and cname not in m.cells:
                problems.append(f"cell {cname!r} is not a cell of the masters")
            for k, (t, v) in enumerate(ss):
                if t != "FRMR":
                    continue
                f = struct.unpack("<I", v)[0]
                if f >> 24 == 0:
                    continue                     # (made in the game: a levelled creature, an item put down)
                refs += 1
                nm = zlow(ss[k + 1][1])
                base = nm[:-8] if len(nm) > 8 and nm[-8:].isdigit() else nm
                got = m.refs.get((f >> 24, f & 0xFFFFFF))
                if got != (cname, base):
                    src = names[(f >> 24) - 1] if f >> 24 <= len(names) else f"master {f >> 24}"
                    problems.append(f"{cname!r}: {nm!r} as {src} #{f & 0xFFFFFF}, which is {got}")
            for t, v in ss:
                if t == "SCRI":
                    need("reference script", zlow(v), {"SCPT"})
        elif tag in ("NPCC", "CREC", "CNTC"):
            need(tag, zlow(d["NAME"]))
            for t, v in ss:
                if t == "NPCO":
                    need("item", zlow(v[4:]))
                elif t == "XSOL":
                    need("soul", zlow(v), {"CREA", "NPC_"})
        elif tag == "SCPT":
            sid = text(d["SCHD"][:32]).lower()
            if sid not in m.schd:
                problems.append(f"global script {sid!r} is not in the masters")
                continue
            src, schd = m.schd[sid]
            if d["SCHD"][32:52] != schd:
                problems.append(f"global script {sid!r}: SCHD is not {src}'s")
            if "SLCS" in d and struct.unpack("<3i", d["SLCS"]) != struct.unpack_from("<3i", schd):
                problems.append(f"global script {sid!r}: locals {struct.unpack('<3i', d['SLCS'])} for {src}'s "
                                f"{struct.unpack_from('<3i', schd)}")
        elif tag == "GLOB":
            need("global", zlow(d["NAME"]), {"GLOB"})
        elif tag == "DIAL":
            need("topic", zlow(d["NAME"]), {"DIAL"})
        elif tag == "NPC_":
            for t, v in ss:
                if t == "NPCS":
                    need("player spell", zlow(v), {"SPEL"})
                elif t == "NPCO":
                    need("player item", zlow(v[4:]))
        elif tag == "PCDT":
            for t, v in ss:
                if t == "DNAM":
                    need("known topic", zlow(v), {"DIAL"})
                elif t == "BNAM":
                    need("birthsign", zlow(v), {"BSGN"})
    return refs, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saves", nargs="+", type=Path)
    args = ap.parse_args()
    bad = 0
    for p in args.saves:
        refs, problems = check(p)
        print(f"{p.name}: {refs} references, " + (f"{len(problems)} problems" if problems else "all good"))
        for line in problems[:40]:
            print("  " + line)
        if len(problems) > 40:
            print(f"  ... and {len(problems) - 40} more")
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
