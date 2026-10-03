"""What the original engine consumes that this port ignores: a first-principles completeness audit.

The game is data plus an engine. Coverage tools (scriptcheck, questcheck) only prove that what scripts and
dialogue *call* works. The engine also acts on things nothing calls: record fields (a creature's services,
an NPC standing on a platform that goes away), game settings (GMSTs: nearly every rule and UI string),
Morrowind.ini sections (moons, blood, level-up art, movies...) and asset folders it loads by convention
(Splash, Video, sky, VFX). Each source is enumerated in full and checked against the port:
  - GMSTs: named anywhere in source/ or tools/ (lowercase or as written)?
  - Morrowind.ini sections and keys: named in tools/?
  - record subrecords, per record type: named in tools/ (the converter is the only reader of the ESM)?
  - archive folders: does converted output or code name anything from them?
Writes build/audit.md: the unused ones, grouped, with counts. Nothing is judged automatically: each
group is a question to answer (implemented another way / not needed / missing).

  python tools/test/audit.py
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from mwfiles import DATA_FILES, Archives, read_records

ROOT = Path(__file__).resolve().parents[2]


def corpus():
    """All source and tool text, lowercase (what the port can possibly know about)."""
    parts = []
    for pat in ("source/*.cpp", "source/*.pica", "include/*.h", "tools/**/*.py"):
        for p in ROOT.glob(pat):
            if p.name == "audit.py":
                continue
            parts.append(p.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts).lower()


def main():
    text = corpus()
    out = ["# Completeness audit (tools/test/audit.py)", ""]

    # ---- GMSTs
    gmsts = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"GMST"}):
        gmsts[r.id] = r
    unused = sorted(g for g in gmsts if g.lower() not in text)
    groups = defaultdict(list)
    for g in unused:
        # group by the word after the type prefix: fFallDamage -> fall, sMagicCorprusWorsens -> magic
        m = re.match(r"[fisb]?([A-Z][a-z]+|[a-z]+)", g)
        groups[(m.group(1) if m else g).lower()].append(g)
    out.append(f"## Game settings (GMST): {len(gmsts) - len(unused)} of {len(gmsts)} named by the port")
    out.append("")
    for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        out.append(f"- **{k}** ({len(v)}): " + ", ".join(v[:40]) + (" ..." if len(v) > 40 else ""))
    out.append("")

    # ---- Morrowind.ini
    ini_path = DATA_FILES.parent / "Morrowind.ini"
    ini = ini_path.read_text(encoding="cp1252", errors="replace") if ini_path.exists() else ""
    sections = re.findall(r"^\[([^\]]+)\]", ini, re.M)
    out.append(f"## Morrowind.ini: {len(sections)} sections")
    out.append("")
    for sname in sections:
        body = re.search(r"^\[%s\]\s*$(.*?)(?=^\[|\Z)" % re.escape(sname), ini, re.M | re.S).group(1)
        keys = [l.split("=", 1)[0].strip() for l in body.splitlines() if "=" in l]
        known_sec = sname.lower() in text or re.sub(r"\s*\d+$", "", sname).lower() in text
        used = [k for k in keys if k.lower() in text]
        mark = "used" if known_sec else "UNUSED"
        out.append(f"- [{sname}] {mark}: {len(used)}/{len(keys)} keys named"
                   + ("" if known_sec else " -- keys: " + ", ".join(keys[:12])))
    out.append("")

    # ---- record subrecords
    per_type = defaultdict(lambda: defaultdict(int))
    for r in read_records(DATA_FILES / "Morrowind.esm", None):
        for tag, _ in r.subs:
            per_type[r.tag][tag] += 1
    out.append("## Record fields (subrecords) the converter never names")
    out.append("")
    for rtype in sorted(per_type):
        miss = [f"{t} ({n})" for t, n in sorted(per_type[rtype].items())
                if f'"{t.lower()}"' not in text and t not in ("NAME", "DELE")]
        if miss:
            out.append(f"- **{rtype}**: " + ", ".join(miss))
    out.append("")

    # ---- archive folders
    arch = Archives()
    folders = defaultdict(int)
    for name in arch.names():
        parts = name.lower().replace("/", "\\").split("\\")
        key = "\\".join(parts[:2]) if len(parts) > 2 else parts[0]
        folders[key] += 1
    out.append("## Archive folders (files) not named by the port")
    out.append("")
    for f, n in sorted(folders.items()):
        leaf = f.split("\\")[-1]
        named = leaf in text or f.replace("\\", "\\\\") in text or f in text
        if not named:
            out.append(f"- {f} ({n})")
    out.append("")

    path = ROOT / "build" / "audit.md"
    path.write_text("\n".join(out), encoding="utf-8")
    print(f"wrote {path}: {len(unused)} GMSTs unnamed, {len(sections)} ini sections")


if __name__ == "__main__":
    main()
