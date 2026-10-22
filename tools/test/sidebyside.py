"""Side-by-side shots: the 3DS engine beside Morrowind's own look (OpenMW 0.51 on the user's Morrowind.esm), 400 x 240
each, no HUD, the same camera, hour and actor pose on both sides. For comparing animations, models and lighting.

    python tools/test/sidebyside.py <shots.txt> [--out build/sidebyside] [--pc-only] [--3ds-only] [--emu] [--data out/world] [--tribunal]

A shots file has one shot per line (# starts a comment), a name then key=value pairs (shell quoting):

    fargoth-idle2  cam=-10511,-71468,300,0,-5  hour=12  actor=fargoth  group=idle2  at=0,0.25,0.5,0.75
    census-office  cell="Seyda Neen, Census and Excise Office"  cam=...

    cam=x,y,z,yaw,pitch   the camera (world units; degrees: yaw 0 looks along +Y, 90 along +X; pitch up positive)
    cell="..."            an interior (left out: the exterior)
    hour=h                the time of day (stopped there)
    weather=name          the weather (Clear, Cloudy, Foggy, Overcast, Rain, Thunderstorm, Ashstorm, Blight, Snow,
                          Blizzard): OpenMW's core.weather.changeWeather on the region, the 3DS's SETWEATHER
    actor=id              the NPC / creature (nearest the camera) sent back to where Morrowind.esm placed it ...
    group=name at=f,...   ... holding that animation group at those points (0 its start key, 1 its stop key):
                          one shot per point, "<name>-<n>"
    face=deg              ... turned that way instead of its placed rotation
    natural=1             ... OpenMW side: no group held, the actor standing in its own idle (a held group plays at
                          Scripted priority, over the arm's torch pose; the 3DS side holds the group)
    view=fp               first person: the player stands at cam (their feet; pitch 0) and their own arms hold the
                          group (a spell readied in OpenMW: spell=id, fire bite unless given; fists for the
                          hand-to-hand and weapon groups, HHChop, 1hSlashF, ...)
    actor=player          third person: the player's own body stands at from=x,y,z,yaw (seen from cam, nobody
                          near) and holds the group (Attack1h, ...)
    weapon=id ammo=id shield=id   the player carries and equips these (fp and actor=player shots)
    wear=id,id,...        armor and clothing the player wears (gauntlets, robes ...), on both sides
    hide=1                both sides hide every person and creature within 4000 units (one wandering into a
                          plain shot of the world)
    spawn=id [dist=d]     a new actor (a creature) 250 (d) units ahead of the camera, facing it, is the shot's actor
    do=step;step... after=t   a scene step, OpenMW side only (--pc-only): goto:<cell>:x:y:z, journal:<quest>:<n>,
                          give:<id>, kill:<id>, group:<id>:<group>, global:<name>:<n>; the shot `after` seconds later,
                          the world going on from there for the next shot (the final scene: tools/test/cases/
                          scratch/final-scene.txt is our side)
    vfx=static pos=x,y,z after=t,...   that spell visual (a STAT the magic effects name) played on its own at pos and
                          the world stopped t seconds after it began: one shot per t (OpenMW: world.vfx.spawn; the 3DS:
                          its VFX step); nobody near; ptex=texture gives it a particle texture (OpenMW's override),
                          effect=n the magic effect whose particle texture the 3DS side gives it (the one with that PTEX);
                          stand=id puts that actor at pos, holding its idle, to wear it (the camera must look at pos)
    cast=id after=t,...   the player casts that spell (level, ahead) and the world stands still t seconds after the
                          cast began: one shot per t (bolts in flight, cast and hit visuals). The player stands at
    from=x,y,z,yaw        from= (their feet, degrees), or at cam with view=fp; skills, Willpower and Luck at 100 and
                          Magicka plenty, so every cast succeeds; 3 seconds between casts for the last one's visuals;
                          every person and creature within 4000 units hidden

Group names are the 3DS engine's (tools/convert/actors.py, firstperson.py); OPENMW_GROUPS says which Morrowind group and
text keys each is in OpenMW (CastSelf: spellcast from "self start" to "self stop").

OpenMW runs from build/openmw-0.51 (unpacked from the release installer) with its own config, saves and logs in
build/openmw-sbs, never the user's. It reads the Morrowind folder from .env (MW3DS_MORROWIND_DIR) and loads
Morrowind.esm only. The 3DS side runs the case it writes (tools/test/cases/scratch/sbs-<file>.txt, git-ignored)
natively (build/native-sbs/mw3ds-native.exe when built there, else build/native; --exe another), or in Azahar with --emu.
Both sides put the player 300 units behind the camera (first person: at it). Writes <out>/pc/<shot>.png, <out>/3ds/<shot>.png and
<out>/<shot>.png (3DS left, OpenMW right).
"""
import argparse
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OPENMW = Path(os.environ.get("MW3DS_OPENMW") or ROOT / "build" / "openmw-0.51")
# (MW3DS_SBS_DIR / MW3DS_SBS_SD: another OpenMW folder and native save folder, so several runs can go at once)
SBS = Path(os.environ.get("MW3DS_SBS_DIR") or ROOT / "build" / "openmw-sbs")
MOD = Path(__file__).resolve().parent / "sidebyside"
W, H = 400, 240
TRIBUNAL = False         # --tribunal: OpenMW loads Tribunal.esm too
SPAWN_DIST = 250         # spawn=: this far ahead of the camera
# 3DS group -> (Morrowind group, start key, stop key) where the engine splits a group by its text keys
OPENMW_GROUPS = {
    "castself": ("spellcast", "self start", "self stop"),
    "casttarget": ("spellcast", "target start", "target stop"),
    "casttouch": ("spellcast", "touch start", "touch stop"),
    "spelleq": ("spellcast", "equip start", "equip stop"),
    "spelluneq": ("spellcast", "unequip start", "unequip stop"),
    "block": ("shield", "block start", "block stop"),
}
# Melee: third person Attack<W> is chop start to its large follow stop (actors.py); first person <W><Kind> the wind-up
# to max attack, <W><Kind>F the blow from min hit to the large follow stop (firstperson.py)
for _tag, _g in (("hh", "handtohand"), ("1h", "weapononehand"), ("2c", "weapontwohand"), ("2w", "weapontwowide")):
    OPENMW_GROUPS["attack" + _tag] = (_g, "chop start", "chop large follow stop")
    OPENMW_GROUPS[_tag + "eq"] = (_g, "equip start", "equip stop")
    OPENMW_GROUPS[_tag + "uneq"] = (_g, "unequip start", "unequip stop")
    for _k in ("chop", "slash", "thrust"):
        OPENMW_GROUPS[_tag + _k] = (_g, f"{_k} start", f"{_k} max attack")
        OPENMW_GROUPS[_tag + _k + "f"] = (_g, f"{_k} min hit", f"{_k} large follow stop")

# Ranged: third person Attack<W> from shoot start to its follow stop (actors.py PHASED_GROUPS); first person <W>Shoot the
# draw to max attack, <W>ShootF the release to the follow stop
for _tag, _g in (("bow", "bowandarrow"), ("xbow", "crossbow"), ("throw", "throwweapon")):
    OPENMW_GROUPS["attack" + _tag] = (_g, "shoot start", "shoot follow stop")
    OPENMW_GROUPS[_tag + "shoot"] = (_g, "shoot start", "shoot max attack")
    OPENMW_GROUPS[_tag + "shootf"] = (_g, "shoot release", "shoot follow stop")
    OPENMW_GROUPS[_tag + "eq"] = (_g, "equip start", "equip stop")
    OPENMW_GROUPS[_tag + "uneq"] = (_g, "unequip start", "unequip stop")
OPENMW_GROUPS["idlexbow"] = ("idlecrossbow", None, None)


def weapon_stance(s):
    """A held group with the weapon (or fists) out rather than a spell readied: anything but a cast."""
    g = s.get("group", "").lower()
    return bool(g) and not g.startswith(("cast", "spell"))


def env_value(key):
    if os.environ.get(key):
        return os.environ[key]
    env = ROOT / ".env"
    if env.exists():
        for line in env.read_text().splitlines():
            if line.startswith(key + "="):
                return line.split("=", 1)[1].strip().strip('"')
    sys.exit(f"sidebyside: {key} not set (.env)")


def read_shots(path):
    """The shots, one per animation point: dicts with name, cell, cam (5 floats), hour, actor, group, at, face."""
    shots = []
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        words = shlex.split(line)
        base = {"name": words[0]}
        for w in words[1:]:
            if "=" not in w:
                sys.exit(f"{path}:{n}: '{w}' is not key=value")
            k, v = w.split("=", 1)
            base[k] = v
        if "cam" not in base:
            sys.exit(f"{path}:{n}: no cam=")
        cam = [float(x) for x in base["cam"].split(",")]
        if len(cam) != 5:
            sys.exit(f"{path}:{n}: cam= wants x,y,z,yaw,pitch")
        base["cam"] = cam
        if base.get("view") == "fp" and cam[4] != 0:
            sys.exit(f"{path}:{n}: a first-person shot looks level (pitch 0)")
        if "spawn" in base:
            # a new one, SPAWN_DIST ahead of the camera, facing it
            base["actor"] = base["spawn"]
            base.setdefault("face", str((cam[3] + 180.0) % 360.0))
        if base.get("actor", "").lower() == "player":
            base["body"] = True
            del base["actor"]
            frm = [float(x) for x in base.get("from", "").split(",") if x]
            if len(frm) != 4:
                sys.exit(f"{path}:{n}: actor=player wants from=x,y,z,yaw (where the player stands)")
            base["from"] = frm
        if "vfx" in base:
            pos = [float(x) for x in base.get("pos", "").split(",") if x]
            if len(pos) != 3:
                sys.exit(f"{path}:{n}: vfx= wants pos=x,y,z (where it plays)")
            base["pos"] = pos
            base["model"] = vfx_model(base["vfx"])
            if "stand" in base:
                # (spawn= puts it dist ahead of the camera: the camera looks at pos)
                base["spawn"] = base["actor"] = base.pop("stand")
                base["dist"] = str(math.hypot(pos[0] - cam[0], pos[1] - cam[1]))
                base.setdefault("face", str((cam[3] + 180.0) % 360.0))
                base.setdefault("group", "idle")
                base["at"] = float(base.get("at", 0.3))
            points = [float(x) for x in base.pop("after", "1").split(",")]
            key = "after"
        elif "do" in base:
            # a scene step (OpenMW side only, --pc-only): the world does it, the shot comes after seconds later, and the
            # next shot goes on from there (nothing set back between them)
            points = [float(x) for x in base.pop("after", "1").split(",")]
            key = "after"
        elif "cast" in base:
            if base.get("view") != "fp" and "from" not in base:
                sys.exit(f"{path}:{n}: a cast seen from cam wants from=x,y,z,yaw (where the player casts)")
            frm = [float(x) for x in base["from"].split(",")] if "from" in base else cam[:4]
            if len(frm) != 4:
                sys.exit(f"{path}:{n}: from= wants x,y,z,yaw")
            base["from"] = frm
            points = [float(x) for x in base.pop("after", "1").split(",")]
            key = "after"
        else:
            points = [float(x) for x in base.pop("at", "0").split(",")]
            key = "at"
        for i, at in enumerate(points):
            s = dict(base, **{key: at})
            if len(points) > 1:
                s["name"] = f"{base['name']}-{i:02d}"
            shots.append(s)
    return shots


_statics = None


def vfx_model(static_id):
    """The model a spell visual static uses (Morrowind.esm), as OpenMW wants it: meshes/..."""
    global _statics
    if _statics is None:
        sys.path.insert(0, str(ROOT / "tools" / "convert"))
        from mwfiles import read_game
        _statics = {r.id.lower(): r.zstr("MODL") for r in read_game({"STAT"})}
    m = _statics.get(static_id.lower())
    if not m:
        # (a few visuals the effects name aren't STAT records)
        from mwfiles import load_db
        rec = load_db()["objects"].get(static_id.lower())
        m = rec.zstr("MODL") if rec is not None else None
    if not m:
        sys.exit(f"sidebyside: no static {static_id}")
    return "meshes/" + m.replace(chr(92), "/").lower()


# ---- OpenMW

def lua_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_openmw_setup(shots):
    """build/openmw-sbs: openmw.cfg, settings.cfg, the Lua mod and this run's poses.lua."""
    mw = Path(env_value("MW3DS_MORROWIND_DIR"))
    data_files = mw / "Data Files"
    if not (data_files / "Morrowind.esm").exists():
        sys.exit(f"sidebyside: no Morrowind.esm in {data_files}")
    data = SBS / "data"
    if data.exists():
        shutil.rmtree(data)
    shutil.copytree(MOD, data)
    lines = ["return {"]
    for s in shots:
        x, y, z, yaw, pitch = s["cam"]
        f = [f"name = {lua_str(s['name'])}", f"x = {x}", f"y = {y}", f"z = {z}",
             f"yaw = {math.radians(yaw)!r}", f"pitch = {math.radians(pitch)!r}"]
        if "cell" in s:
            f.append(f"cell = {lua_str(s['cell'])}")
        if "hour" in s:
            f.append(f"hour = {float(s['hour'])}")
        if "weather" in s:
            f.append(f"weather = {lua_str(s['weather'])}")
        if "actor" in s:
            f.append(f"actor = {lua_str(s['actor'].lower())}")
        if "group" in s and not s.get("natural"):
            g = s["group"].lower()
            group, start, stop = OPENMW_GROUPS.get(g, (g, None, None))
            f.append(f"group = {lua_str(group)}")
            if start:
                f.append(f"startKey = {lua_str(start)}, stopKey = {lua_str(stop)}")
            f.append(f"at = {s['at']}")
        if s.get("view") == "fp":
            f.append("fp = true")
        if s.get("hide"):
            f.append("hide = true")
        if s.get("body"):
            fx, fy, fz, fyaw = s["from"]
            f.append(f"body = true, fromX = {fx}, fromY = {fy}, fromZ = {fz}, fromYaw = {math.radians(fyaw)!r}")
        if (s.get("view") == "fp" or s.get("body")) and weapon_stance(s):
            f.append("stance = \"weapon\"")
        if "weapon" in s:
            f.append(f"weapon = {lua_str(s['weapon'].lower())}")
        for k in ("ammo", "shield"):
            if k in s:
                f.append(f"{k} = {lua_str(s[k].lower())}")
        if "wear" in s:
            f.append("wear = { " + ", ".join(lua_str(w.lower()) for w in s["wear"].split(",")) + " }")
        if "spell" in s:
            f.append(f"spell = {lua_str(s['spell'].lower())}")
        if "face" in s:
            f.append(f"face = {math.radians(float(s['face']))!r}")
        if "spawn" in s:
            f.append(f"spawn = {float(s.get('dist', SPAWN_DIST))}")
        if "vfx" in s:
            px, py, pz = s["pos"]
            f.append(f"vfx = {lua_str(s['model'])}, after = {s['after']}, vfxX = {px}, vfxY = {py}, vfxZ = {pz}")
            if "ptex" in s:
                f.append(f"ptex = {lua_str(s['ptex'])}")
        if "do" in s:
            f.append(f"act = {lua_str(s['do'])}, after = {s['after']}")
        if "cast" in s:
            fx, fy, fz, fyaw = s["from"]
            f.append(f"cast = {lua_str(s['cast'].lower())}, after = {s['after']}")
            f.append(f"fromX = {fx}, fromY = {fy}, fromZ = {fz}, fromYaw = {math.radians(fyaw)!r}")
        lines.append("\t{ " + ", ".join(f) + " },")
    lines.append("}")
    (data / "scripts" / "mw3ds_sbs" / "poses.lua").write_text("\n".join(lines) + "\n")
    user = SBS / "user"
    user.mkdir(parents=True, exist_ok=True)

    def q(p):
        return '"' + str(p).replace("\\", "/") + '"'
    # (--replace=config leaves out the install's own openmw.cfg: its Morrowind Lua scripts, resources/vfs-mw: combat,
    # music, cell handlers; and OpenMW's default Morrowind.ini values, fallback=. The game's own Morrowind.ini is
    # imported over those below: the ash storm's and blight's skies are only there)
    install = (OPENMW / "openmw.cfg").read_text(errors="replace").splitlines()
    fallbacks = [ln for ln in install if ln.startswith("fallback=")]
    (SBS / "openmw.cfg").write_text("\n".join([
        "# Written by tools/test/sidebyside.py",
        f"data={q(OPENMW / 'resources' / 'vfs-mw')}",
        f"data={q(data_files)}",
        f"data={q(data)}",
        "fallback-archive=Morrowind.bsa"] + (["fallback-archive=Tribunal.bsa"] if TRIBUNAL else []) + [
        "content=Morrowind.esm"] + (["content=Tribunal.esm"] if TRIBUNAL else []) + [
        "content=mw3ds_sbs.omwscripts",
        f"user-data={q(user)}"] + fallbacks + [""]))
    ini = mw / "Morrowind.ini"
    importer = OPENMW / "openmw-iniimporter.exe"
    if ini.exists() and importer.exists():
        # (-A: no archives from it; Morrowind.esm's own is set above)
        r = subprocess.run([str(importer), "-A", "-i", str(ini), "-c", str(SBS / "openmw.cfg")],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("sidebyside: Morrowind.ini not imported:", (r.stderr or r.stdout).strip()[-300:])
    # The 3DS's top screen: 400 x 240, a 60 degree vertical field of view (mwrender/renderingmanager.cpp kFovY)
    (SBS / "settings.cfg").write_text("\n".join([
        "# Written by tools/test/sidebyside.py",
        "[Video]", "resolution x = 400", "resolution y = 240", "window mode = 2", "window border = true",
        "vsync mode = 0", "framerate limit = 60", "",
        "[Camera]", "field of view = 60", "",
        "[General]", "screenshot format = png", "",
        "[Game]", "", ""]))
    return user


def find_window(pid):
    import win32gui
    import win32process
    found = []

    def cb(hwnd, _):
        if win32gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1] == pid:
            found.append(hwnd)
    win32gui.EnumWindows(cb, None)
    return found[0] if found else None


def press_f12(hwnd):
    import win32api
    import win32con
    scan = 0x58
    win32api.PostMessage(hwnd, win32con.WM_KEYDOWN, win32con.VK_F12, 1 | (scan << 16))
    time.sleep(0.05)
    win32api.PostMessage(hwnd, win32con.WM_KEYUP, win32con.VK_F12, 1 | (scan << 16) | 0xC0000000)


def run_openmw(shots, out, timeout):
    exe = OPENMW / "openmw.exe"
    if not exe.exists():
        sys.exit(f"sidebyside: no {exe} (unpack OpenMW-0.51.0-Windows-x64.exe there)")
    user = write_openmw_setup(shots)
    shots_dir = user / "screenshots"
    log = SBS / "openmw.log"
    for f in (log, user / "openmw.log"):
        if f.exists():
            f.unlink()
    out.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), "--replace=config", f"--config={SBS}", "--skip-menu", "--start=Seyda Neen", "--no-grab",
           "--no-sound"]
    proc = subprocess.Popen(cmd, cwd=OPENMW)
    print(f"openmw: pid {proc.pid}")
    try:
        deadline = time.time() + timeout
        done, seen = False, 0
        while time.time() < deadline and not done:
            if proc.poll() is not None:
                print(f"openmw: exited ({proc.returncode})")
                break
            logs = [f for f in (log, user / "openmw.log") if f.exists()]
            if not logs:
                time.sleep(0.5)
                continue
            lines = [ln for ln in logs[0].read_text(errors="replace").splitlines() if "MW3DS-SBS" in ln]
            for ln in lines[seen:]:
                print("openmw:", ln.split("MW3DS-SBS", 1)[1].strip())
                parts = ln.split("MW3DS-SBS", 1)[1].split()
                if parts[0] == "done":
                    done = True
                elif parts[0] == "ready":
                    name = shots[int(parts[1]) - 1]["name"]
                    before = set(shots_dir.glob("*.png")) if shots_dir.exists() else set()
                    hwnd = find_window(proc.pid)
                    press_f12(hwnd)
                    new = []
                    for _ in range(100):
                        time.sleep(0.1)
                        new = sorted(set(shots_dir.glob("*.png")) - before) if shots_dir.exists() else []
                        if new:
                            break
                    if not new:
                        print(f"openmw: no screenshot for {name}")
                        continue
                    # (written asynchronously: under load it can still be half written, so try again a while)
                    img = None
                    for _ in range(20):
                        time.sleep(0.3)
                        try:
                            img = Image.open(new[-1]).convert("RGB")
                            break
                        except OSError:
                            img = None
                    if img is None:
                        print(f"openmw: screenshot for {name} never finished")
                        continue
                    if img.size != (W, H):
                        print(f"openmw: screenshot is {img.size[0]} x {img.size[1]}, scaled to {W} x {H}")
                        img = img.resize((W, H), Image.LANCZOS)
                    img.save(out / f"{name}.png")
                    new[-1].unlink()
            seen = len(lines)
            time.sleep(0.2)
        if not done:
            print("openmw: did not finish")
    finally:
        if proc.poll() is None:
            proc.kill()


# ---- The 3DS engine

def fmt(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


# Morrowind's weathers in order: the 3DS's SETWEATHER takes the number
WEATHERS = ["clear", "cloudy", "foggy", "overcast", "rain", "thunderstorm", "ashstorm", "blight", "snow", "blizzard"]


def weather_step(s):
    """The 3DS side's SETWEATHER for the shot's weather= (by name or number)."""
    w = s["weather"].strip().lower()
    return f"SETWEATHER:{WEATHERS.index(w) if w in WEATHERS else int(w)}"


def write_case(shots, name):
    """The test case: per shot the player behind the camera, the clock, then CAM + HUD:off + POSE, then SHOT."""
    # (the player a Dark Elf man, as OpenMW's default player: their first-person arms)
    lines = ["# start: Seyda Neen", "# Written by tools/test/sidebyside.py (side-by-side shots with OpenMW)",
             "0.5 0 0 0 0 FRAMECAP:off+PCRACE:dark_elf+PCSEX:0"]
    for i, s in enumerate(shots):
        if "cast" in s:
            lines += cast_steps(s)
            continue
        if "vfx" in s:
            lines += vfx_steps(s)
            continue
        if s.get("body"):
            lines += body_steps(s)
            continue
        x, y, z, yaw, pitch = s["cam"]
        fp = s.get("view") == "fp"
        # (the last point just short of the stop key, as OpenMW can hold it: hold.lua's kLast)
        at = f"{min(s['at'], 0.998):.3f}"
        # (first person: the player stands at cam; else 300 behind it, out of the picture)
        bx, by = (x, y) if fp else (x - math.sin(math.radians(yaw)) * 300, y - math.cos(math.radians(yaw)) * 300)
        setup = []
        if "cell" in s:
            lines.append(f"0.5 0 0 0 0 GOTO:{s['cell'].replace(' ', '_')}")
            setup.append(f"TPIN:{fmt(bx)}:{fmt(by)}:{fmt(z)}:{fmt(yaw)}")
        else:
            setup.append(f"TP:{fmt(bx)}:{fmt(by)}:{fmt(z)}:{fmt(yaw)}")
        if "hour" in s:
            setup.append(f"HOUR:{s['hour']}")
        lines.append("1 0 0 0 0 " + "+".join(setup))
        lines += gear(s)
        if "spawn" in s:
            # nobody else near, then the creature put there (shown again: the hiding caught it too)
            sid = s["spawn"].replace(" ", "%")
            lines += [f"0.3 0 0 0 0 HIDEACTORS:{HIDE_RADIUS}", f"1 0 0 0 0 PLACE:{sid}:{fmt(300 + float(s.get('dist', SPAWN_DIST)))}",
                      f"0.1 0 0 0 0 ENABLE:{sid}"]
        if fp:
            view = ["CAM:off", "POV:first", "HUD:off"] + ([f"VMPOSE:{s['group']}:{at}"] if "group" in s else [])
        else:
            view = [f"CAM:{fmt(x)}:{fmt(y)}:{fmt(z)}:{fmt(yaw)}:{fmt(pitch)}", "POV:first", "HUD:off", "VMPOSE:off"]
        if s.get("hide"):
            view.append(f"HIDEACTORS:{HIDE_RADIUS}")
        if "weather" in s:
            view.append(weather_step(s))      # (after the teleport: the region it lands in)
        if not fp and "actor" in s and "group" in s:
            pose = f"POSE:{s['actor'].replace(' ', '%')}:{s['group']}:{at}"
            if "face" in s:
                pose += f":{s['face']}"
            view.append(pose)
        lines.append("1 0 0 0 0 " + "+".join(view))
        lines.append("0.5 0 0 0 0 SHOT")
        if "spawn" in s and (i + 1 == len(shots) or shots[i + 1].get("spawn") != s["spawn"]):
            lines.append(f"0.1 0 0 0 0 DISABLE:{s['spawn'].replace(' ', '%')}")
    case = ROOT / "tools" / "test" / "cases" / "scratch" / f"{name}.txt"
    case.parent.mkdir(parents=True, exist_ok=True)
    case.write_text("\n".join(lines) + "\n")
    return case


# (every cast succeeds and none runs short of Magicka: global.lua sets the same in OpenMW)
CAST_SKILLS = ["destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration"]
# (and nobody near: no one walks through the picture or catches the bolt; global.lua hides the same)
HIDE_RADIUS = 4000


def gear(s):
    """Steps that GIVE (add and equip) the shot's weapon, ammunition, shield and worn items: one GIVE a step."""
    ids = [s[k] for k in ("weapon", "ammo", "shield") if k in s] + (s["wear"].split(",") if "wear" in s else [])
    return [f"0.2 0 0 0 0 GIVE:{i.replace(' ', '_')}" for i in ids]


def body_steps(s):
    """The player's body in third person at from=, seen from cam, holding the group (BODYPOSE), nobody near."""
    x, y, z, yaw, pitch = s["cam"]
    fx, fy, fz, fyaw = s["from"]
    setup = [f"TP:{fmt(fx)}:{fmt(fy)}:{fmt(fz)}:{fmt(fyaw)}"]
    if "hour" in s:
        setup.append(f"HOUR:{s['hour']}")
    view = [f"CAM:{fmt(x)}:{fmt(y)}:{fmt(z)}:{fmt(yaw)}:{fmt(pitch)}", "POV:third", "HUD:off", "VMPOSE:off",
            f"HIDEACTORS:{HIDE_RADIUS}"]
    if "weather" in s:
        view.append(weather_step(s))
    at = f"{min(s['at'], 0.998):.3f}"
    return ["1 0 0 0 0 " + "+".join(setup)] + gear(s) + ["1 0 0 0 0 " + "+".join(view),
            f"0.5 0 0 0 0 BODYPOSE:{s['group']}:{at}", "0.5 0 0 0 0 SHOT", "0.1 0 0 0 0 BODYPOSE:off"]


def cast_steps(s):
    """A cast shot: the player at from=, the view (theirs, or cam), the spell cast, the world stopped `after` seconds
    later for the SHOT, then going on long enough for the bolt and its visuals to be gone."""
    x, y, z, yaw, pitch = s["cam"]
    fx, fy, fz, fyaw = s["from"]
    setup = [f"TP:{fmt(fx)}:{fmt(fy)}:{fmt(fz)}:{fmt(fyaw)}"]
    if "hour" in s:
        setup.append(f"HOUR:{s['hour']}")
    fp = s.get("view") == "fp"
    # (seen from cam, the player is in the picture: their body, in third person)
    view = ["CAM:off" if fp else f"CAM:{fmt(x)}:{fmt(y)}:{fmt(z)}:{fmt(yaw)}:{fmt(pitch)}",
            "POV:first" if fp else "POV:third", "HUD:off", "VMPOSE:off", "SETMAGICKA:1000",
            "SETATTR:willpower:100", "SETATTR:luck:100",
            f"HIDEACTORS:{HIDE_RADIUS}"]
    if "weather" in s:
        view.append(weather_step(s))
    view += [f"SETSKILL:{k}:100" for k in CAST_SKILLS]
    return ["1 0 0 0 0 " + "+".join(setup), "1 0 0 0 0 " + "+".join(view),
            f"{fmt(s['after'])} 0 0 0 0 CAST:{s['cast'].replace(' ', '_')}",
            "0.1 0 0 0 0 HALT:on", "0.5 0 0 0 0 SHOT", "3 0 0 0 0 HALT:off"]


def vfx_steps(s):
    """A spell visual on its own: the player 300 behind the camera, nobody near, the visual played at pos, the world
    stopped `after` seconds later for the SHOT, then going on for it to be gone."""
    x, y, z, yaw, pitch = s["cam"]
    bx, by = x - math.sin(math.radians(yaw)) * 300, y - math.cos(math.radians(yaw)) * 300
    setup = [f"TP:{fmt(bx)}:{fmt(by)}:{fmt(z)}:{fmt(yaw)}"]
    if "hour" in s:
        setup.append(f"HOUR:{s['hour']}")
    view = [f"CAM:{fmt(x)}:{fmt(y)}:{fmt(z)}:{fmt(yaw)}:{fmt(pitch)}", "POV:first", "HUD:off", "VMPOSE:off",
            f"HIDEACTORS:{HIDE_RADIUS}"]
    if "weather" in s:
        view.append(weather_step(s))
    px, py, pz = s["pos"]
    stand = []
    if "spawn" in s:
        # the actor wearing it, put at pos itself (PLACE in front of the player missed it by up to ~20 units where a
        # wall had nudged the player), holding its idle
        sid = s["spawn"].replace(" ", "%")
        stand = [f"1 0 0 0 0 PLACEAT:{sid}:{fmt(px)}:{fmt(py)}:{fmt(pz)}", f"0.1 0 0 0 0 ENABLE:{sid}",
                 f"0.5 0 0 0 0 POSE:{s['spawn'].replace(' ', '%')}:{s['group']}:{min(s['at'], 0.998):.3f}:{s['face']}"]
    return ["1 0 0 0 0 " + "+".join(setup), "1 0 0 0 0 " + "+".join(view)] + stand + [
            f"{fmt(s['after'])} 0 0 0 0 VFX:{s['vfx'].replace(' ', '_')}:{fmt(px)}:{fmt(py)}:{fmt(pz)}"
            + (f":{int(s['effect'])}" if "effect" in s else ""),
            "0.1 0 0 0 0 HALT:on", "0.5 0 0 0 0 SHOT", "3 0 0 0 0 HALT:off"] + (
            [f"0.1 0 0 0 0 DISABLE:{s['spawn'].replace(' ', '%')}"] if "spawn" in s else [])


def run_3ds(shots, out, name, emu, data, exe):
    case = write_case(shots, name)
    raw = out / "3ds-raw"
    if raw.exists():
        shutil.rmtree(raw)
    cmd = [sys.executable, str(ROOT / "tools" / "test" / "run_test.py"), f"scratch/{name}",
           "--data", data, "--start", "Seyda Neen", "--shots", str(raw)]
    if emu:
        cmd += ["--fast", "--wait", "900"]
    else:
        if not exe:
            own = ROOT / "build" / "native-sbs" / "mw3ds-native.exe"
            exe = str(own if own.exists() else ROOT / "build" / "native" / "mw3ds-native.exe")
        cmd += ["--native", "--exe", exe, "--sd", os.environ.get("MW3DS_SBS_SD") or str(ROOT / "build" / "native-sbs" / "sd")]
    print("3ds:", case.relative_to(ROOT), "emulator" if emu else "native")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        if "RESULT" in ln or "FAIL" in ln or "holds" in ln:
            print("3ds:", ln.strip())
    # (by step number: as text, step 100 would sort before step 99)
    pngs = sorted(raw.glob("shot_step_*.png"), key=lambda p: [int(t) for t in re.findall(r"\d+", p.stem)])
    if len(pngs) != len(shots):
        print(f"3ds: {len(pngs)} shots for {len(shots)} wanted")
    (out / "3ds").mkdir(parents=True, exist_ok=True)
    for s, png in zip(shots, pngs):
        Image.open(png).convert("RGB").crop((0, 0, W, H)).save(out / "3ds" / f"{s['name']}.png")


# ---- Putting them together

def compose(shots, out):
    for s in shots:
        a, b = out / "3ds" / f"{s['name']}.png", out / "pc" / f"{s['name']}.png"
        img = Image.new("RGB", (W * 2, H), (0, 0, 0))
        for i, (p, label) in enumerate(((a, "3DS"), (b, "OpenMW"))):
            if p.exists():
                img.paste(Image.open(p).convert("RGB").resize((W, H)), (W * i, 0))
            d = ImageDraw.Draw(img)
            d.rectangle((W * i + 4, 4, W * i + 8 + 7 * len(label), 18), fill=(0, 0, 0))
            d.text((W * i + 6, 5), label, fill=(255, 255, 255))
        img.save(out / f"{s['name']}.png")
        print(f"sidebyside: {out / (s['name'] + '.png')}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shots")
    ap.add_argument("--out", default=str(ROOT / "build" / "sidebyside"))
    ap.add_argument("--pc-only", action="store_true")
    ap.add_argument("--3ds-only", dest="ds_only", action="store_true")
    ap.add_argument("--emu", action="store_true", help="the 3DS side in Azahar rather than the native build")
    ap.add_argument("--data", default="")
    ap.add_argument("--tribunal", action="store_true",
                    help="the Tribunal add-on on both sides: OpenMW loads Tribunal.esm, the 3DS side out/tribunal")
    ap.add_argument("--exe", default="", help="another native build")
    ap.add_argument("--timeout", type=float, default=0, help="seconds for the OpenMW run (default 60 + 15 a shot)")
    a = ap.parse_args()
    shots = read_shots(a.shots)
    out = Path(a.out)
    global TRIBUNAL
    TRIBUNAL = a.tribunal
    a.data = a.data or ("out/tribunal" if a.tribunal else "out/world")
    if not a.pc_only:
        run_3ds(shots, out, "sbs-" + Path(a.shots).stem + "-" + out.name, a.emu, a.data, a.exe)
    if not a.ds_only:
        run_openmw(shots, out / "pc", a.timeout or 60 + 15 * len(shots))
    compose(shots, out)


if __name__ == "__main__":
    main()
