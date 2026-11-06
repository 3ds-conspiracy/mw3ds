"""Golden run, OpenMW's half: ask a real OpenMW 0.51 what it works out, and save the answers.

    python tools/test/golden/golden_openmw.py actors [--timeout 1800] [--from-log] [--out build/golden/openmw-actors.json]

Topic "actors": every NPC and creature record of Morrowind.esm is made next to the player in turn, and OpenMW's own
numbers are dumped: level, the eight attributes, health / magicka / fatigue, the AI settings (fight, flee, alarm,
hello); for an NPC also the 27 skills and its gold, for a creature its soul value and three attacks. An NPC with the
Autocalc flag has all of its stats worked out from its race, class and level: the rule the converter
(tools/convert/npcstats.py) has to agree with.
golden_compare.py then sets the answers beside what the converter wrote.

OpenMW runs from build/openmw-0.51 (the same unpacked release tools/test/sidebyside.py uses; MW3DS_OPENMW names
another) with its own config and log in build/golden/openmw, never the user's. It reads the Morrowind folder from .env
(MW3DS_MORROWIND_DIR) and loads Morrowind.esm only. The answers are derived from the game's data, so they stay under
build/ (git-ignored): never commit them.
"""
import argparse
import json
import re
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import mwtools  # noqa: E402

OPENMW = Path(os.environ.get("MW3DS_OPENMW") or ROOT / "build" / "openmw-0.51")
WORK = ROOT / "build" / "golden" / "openmw"
MOD = Path(__file__).resolve().parent / "mod"
TAG = "MW3DS-GOLDEN"


def q(p):
    return '"' + str(p).replace("\\", "/") + '"'


GMSTF = re.compile(r'(?<![A-Za-z0-9_])gmstf\("([A-Za-z0-9_]+)"(?:\s*,\s*([-0-9.eE+]+)f?)?')


def gmst_names():
    """{name: [the defaults the engine's source gives it]} for every numeric game setting it reads (gmstf)."""
    out = {}
    for f in (ROOT / "source").rglob("*.[ch]pp"):
        for name, default in GMSTF.findall(f.read_text(errors="replace")):
            out.setdefault(name.lower(), set())
            if default:
                out[name.lower()].add(float(default))
    return out


def write_setup():
    """build/golden/openmw: openmw.cfg, settings.cfg and a copy of the Lua mod. Returns the user-data folder."""
    mw = Path(mwtools.get_env("MW3DS_MORROWIND_DIR", "the Morrowind install"))
    data_files = mw / "Data Files"
    if not (data_files / "Morrowind.esm").exists():
        sys.exit(f"golden: no Morrowind.esm in {data_files}")
    data = WORK / "data"
    if data.exists():
        shutil.rmtree(data)
    shutil.copytree(MOD, data)
    # the numeric game settings the engine reads (gmstf in its source): OpenMW prints its value of each
    names = sorted(gmst_names())
    lua = ["return {"] + [f'\t"{n}",' for n in names] + ["}", ""]
    (data / "scripts" / "mw3ds_golden" / "gmst_names.lua").write_text("\n".join(lua))
    user = WORK / "user"
    user.mkdir(parents=True, exist_ok=True)
    # (--replace=config leaves out the install's own openmw.cfg; its fallback= lines, the Morrowind.ini values OpenMW
    # defaults to, are put back)
    install = (OPENMW / "openmw.cfg").read_text(errors="replace").splitlines()
    fallbacks = [ln for ln in install if ln.startswith("fallback=")]
    (WORK / "openmw.cfg").write_text("\n".join([
        "# Written by tools/test/golden/golden_openmw.py",
        f"data={q(OPENMW / 'resources' / 'vfs-mw')}",
        f"data={q(data_files)}",
        f"data={q(data)}",
        "fallback-archive=Morrowind.bsa",
        "content=Morrowind.esm",
        "content=mw3ds_golden.omwscripts",
        f"user-data={q(user)}"] + fallbacks + [""]))
    (WORK / "settings.cfg").write_text("\n".join([
        "# Written by tools/test/golden/golden_openmw.py",
        "[Video]", "resolution x = 400", "resolution y = 240", "window mode = 2", "window border = true",
        "vsync mode = 0", "framerate limit = 0", "",
        "[General]", "screenshot format = png", ""]))
    return user


def running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq openmw.exe"], capture_output=True, text=True)
    return "openmw.exe" in r.stdout


def run_openmw(timeout, prepare=None, extra=()):
    """Run OpenMW until the mod says it is done; the mod's lines (after the tag), in order. prepare: called once the
    setup is written (a topic's generated files); extra: more command-line options."""
    exe = OPENMW / "openmw.exe"
    if not exe.exists():
        sys.exit(f"golden: no {exe} (unpack OpenMW-0.51.0-Windows-x64.exe there)")
    if running():
        sys.exit("golden: an openmw.exe is already running (a side-by-side run?): finish it first")
    user = write_setup()
    if prepare:
        prepare()
    logs = [WORK / "openmw.log", user / "openmw.log"]
    for f in logs:
        if f.exists():
            f.unlink()
    cmd = [str(exe), "--replace=config", f"--config={WORK}", "--skip-menu", "--start=Seyda Neen", "--no-grab",
           "--no-sound", *extra]
    proc =subprocess.Popen(cmd, cwd=OPENMW)
    print(f"openmw: pid {proc.pid}")
    lines, done = [], False
    try:
        deadline = time.time() + timeout
        seen = 0
        while time.time() < deadline and not done:
            time.sleep(1)
            have = [f for f in logs if f.exists()]
            if have:
                got = [ln.split(TAG, 1)[1].strip("\r\n ") for ln in have[0].read_text(errors="replace").splitlines()
                       if TAG in ln]
                for ln in got[seen:]:
                    if ln.startswith(("records", "skipped", "done")):
                        print("openmw:", ln)
                    done = done or ln.startswith("done")
                lines, seen = got, len(got)
                if seen and seen % 500 == 0:
                    print(f"openmw: {seen} lines")
            if proc.poll() is not None:
                break
        if not done:
            print("golden: OpenMW stopped before the mod was done (log:", have[0] if have else "none", ")")
    finally:
        for _ in range(20):
            if proc.poll() is not None:
                break
            time.sleep(1)
        if proc.poll() is None:
            proc.kill()
    return lines if done else None


def ints(text):
    return [round(float(x)) for x in text.split(",")]


def actors(lines):
    """{"npc": {id: numbers}, "creature": {id: numbers}, "spells": {npc id: [[spell id, type], ...]}} from the mod's
    lines (type: core.magic.SPELL_TYPE, 0 a spell, 1 an ability, 5 a power)."""
    rows = {"npc": {}, "creature": {}, "spells": {}, "effects": {}, "items": {}, "cspells": {}, "itemfx": {},
            "signs": {}, "levrec": {}, "levpick": {}}
    for ln in lines:
        f = ln.split("\t") + ["", "", ""]          # (a line's empty last field is lost to the strip)
        if f[0] in ("factionrec", "classrec", "racerec", "regionrec"):
            rows.setdefault(f[0], {})[f[1].lower()] = f[2:]
        elif f[0] == "jinfo":
            rows.setdefault("jinfo", {}).setdefault(f[1].lower(), []).append([int(float(f[2])), int(f[3]), int(f[4])])
        elif f[0] in ("spellrec", "enchrec"):
            rows.setdefault(f[0], {})[f[1].lower()] = {"head": [float(x) for x in f[2].split(",")],
                                                      "effects": [e.split(",") for e in f[3].split(";") if e]}
        elif f[0] == "equip":
            rows.setdefault("equip", {}).setdefault(f[1].lower(), {})[f[2]] = f[3].lower()
        elif f[0] == "faction":
            rows.setdefault("faction", {}).setdefault(f[1].lower(), {})[f[2].lower()] = f[3]
        elif f[0] == "rep":
            rows.setdefault("rep", {})[f[1].lower()] = f[2]
        elif f[0] == "gmst":
            rows.setdefault("gmst", {})[f[1].lower()] = f[2]
        elif f[0] == "door":
            rows.setdefault("doors", []).append(f[1:6])
        elif f[0] == "cellobj":
            rows.setdefault("cellobj", {}).setdefault(f[1], {})[f[2].lower()] = int(f[3])
        elif f[0] == "cspell":
            rows["cspells"].setdefault(f[1].lower(), []).append([f[2].lower(), int(float(f[3]))])
        elif f[0] == "itemfx":
            rows["itemfx"][f[2].lower()] = dict(kind=f[1], effects=[e.split(",") for e in f[3].split(";") if e])
        elif f[0] == "sign":
            rows["signs"][f[1].lower()] = sorted(s.lower() for s in f[2].split(",") if s)
        elif f[0] == "levrec":
            rows["levrec"][f[1].lower()] = {"none": float(f[2]), "all": int(f[3]),
                                            "entries": [e.rsplit(":", 1) for e in f[4].split(",") if e]}
        elif f[0] == "levpick":
            rows["levpick"].setdefault(f[1].lower(), {})[f[2]] = sorted(set(s.lower() for s in f[3].split(",")))
        elif f[0] == "item":
            kv = dict(p.split("=", 1) for p in f[3].split(";") if "=" in p)
            rows["items"][f[2].lower()] = dict(kind=f[1], **kv)
        elif f[0] == "effect":
            names = ["school", "cost", "duration", "magnitude", "harmful", "continuousvfx", "self", "touch", "target",
                     "appliedonce", "spellmaking", "enchanting", "negativelight", "skill", "attribute",
                     "casterlinked", "nonrecastable", "unreflectable"]
            v = dict(zip(names, f[3:]))
            rows["effects"][f[2].lower()] = dict({k: (v[k].lower() if k == "school" else float(v[k]) if k == "cost"
                                                      else int(v[k])) for k in names}, key=f[1])
        elif f[0] == "spell":
            rows["spells"].setdefault(f[1], []).append([f[2].lower(), int(float(f[3]))])
        elif f[0] == "npc":
            _, rid, auto, level, attrs, skills, hp, mp, fat, fight, flee, alarm, hello, gold = f[:14]
            rows["npc"][rid] = {"autocalc": int(auto), "level": int(level), "attributes": ints(attrs),
                                "skills": ints(skills), "health": int(float(hp)), "magicka": int(float(mp)),
                                "fatigue": int(float(fat)), "fight": int(float(fight)), "flee": int(float(flee)),
                                "alarm": int(float(alarm)), "hello": int(float(hello)), "gold": int(float(gold))}
        elif f[0] == "creature":
            _, rid, level, attrs, hp, mp, fat, fight, flee, alarm, hello, soul, attack = f[:13]
            rows["creature"][rid] = {"level": int(level), "attributes": ints(attrs), "health": int(float(hp)),
                                     "magicka": int(float(mp)), "fatigue": int(float(fat)),
                                     "fight": int(float(fight)), "flee": int(float(flee)),
                                     "alarm": int(float(alarm)), "hello": int(float(hello)),
                                     "soul": int(float(soul)), "attack": ints(attack)}
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("topic", choices=["actors"])
    ap.add_argument("--timeout", type=int, default=1800, help="seconds to give OpenMW")
    ap.add_argument("--from-log", action="store_true", help="read the last run's log, without running OpenMW again")
    ap.add_argument("--out", type=Path, default=None, help="the answers (default: build/golden/openmw-<topic>.json)")
    args = ap.parse_args()
    if args.from_log:
        text = (WORK / "openmw.log").read_text(errors="replace")
        lines = [ln.split(TAG, 1)[1].strip() for ln in text.splitlines() if TAG in ln]
    else:
        lines = run_openmw(args.timeout)
    if lines is None:
        sys.exit(1)
    rows = actors(lines)
    out = args.out or ROOT / "build" / "golden" / f"openmw-{args.topic}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rows, indent=0, sort_keys=True))
    print(f"golden: {len(rows['npc'])} NPCs and {len(rows['creature'])} creatures written to {out}")


if __name__ == "__main__":
    main()
