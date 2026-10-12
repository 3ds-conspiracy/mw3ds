"""Side-by-side shots: the 3DS engine beside Morrowind's own look (OpenMW 0.51 on the user's Morrowind.esm), 400 x 240
each, no HUD, the same camera, hour and actor pose on both sides. For comparing animations, models and lighting.

    python tools/test/sidebyside.py <shots.txt> [--out build/sidebyside] [--pc-only] [--3ds-only] [--emu] [--data out/world]

A shots file has one shot per line (# starts a comment), a name then key=value pairs (shell quoting):

    fargoth-idle2  cam=-10511,-71468,300,0,-5  hour=12  actor=fargoth  group=idle2  at=0,0.25,0.5,0.75
    census-office  cell="Seyda Neen, Census and Excise Office"  cam=...

    cam=x,y,z,yaw,pitch   the camera (world units; degrees: yaw 0 looks along +Y, 90 along +X; pitch up positive)
    cell="..."            an interior (left out: the exterior)
    hour=h                the time of day (stopped there)
    actor=id              the NPC / creature (nearest the camera) sent back to where Morrowind.esm placed it ...
    group=name at=f,...   ... holding that animation group at those points (0 its start key, 1 its stop key):
                          one shot per point, "<name>-<n>"
    face=deg              ... turned that way instead of its placed rotation

OpenMW runs from build/openmw-0.51 (unpacked from the release installer) with its own config, saves and logs in
build/openmw-sbs, never the user's. It reads the Morrowind folder from .env (MW3DS_MORROWIND_DIR) and loads
Morrowind.esm only. The 3DS side runs the case it writes (tools/test/cases/scratch/sbs-<file>.txt, git-ignored)
natively (build/native-sbs/mw3ds-native.exe when built there, else build/native; --exe another), or in Azahar with --emu.
Both sides put the player 300 units behind the camera. Writes <out>/pc/<shot>.png, <out>/3ds/<shot>.png and
<out>/<shot>.png (3DS left, OpenMW right).
"""
import argparse
import math
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OPENMW = ROOT / "build" / "openmw-0.51"
SBS = ROOT / "build" / "openmw-sbs"
MOD = Path(__file__).resolve().parent / "sidebyside"
W, H = 400, 240


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
        points = [float(x) for x in base.pop("at", "0").split(",")]
        for i, at in enumerate(points):
            s = dict(base, at=at)
            if len(points) > 1:
                s["name"] = f"{base['name']}-{i:02d}"
            shots.append(s)
    return shots


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
        if "actor" in s:
            f.append(f"actor = {lua_str(s['actor'].lower())}")
        if "group" in s:
            f.append(f"group = {lua_str(s['group'].lower())}")
            f.append(f"at = {s['at']}")
        if "face" in s:
            f.append(f"face = {math.radians(float(s['face']))!r}")
        lines.append("\t{ " + ", ".join(f) + " },")
    lines.append("}")
    (data / "scripts" / "mw3ds_sbs" / "poses.lua").write_text("\n".join(lines) + "\n")
    user = SBS / "user"
    user.mkdir(parents=True, exist_ok=True)

    def q(p):
        return '"' + str(p).replace("\\", "/") + '"'
    (SBS / "openmw.cfg").write_text("\n".join([
        "# Written by tools/test/sidebyside.py",
        f"data={q(data_files)}",
        f"data={q(data)}",
        "fallback-archive=Morrowind.bsa",
        "content=Morrowind.esm",
        "content=mw3ds_sbs.omwscripts",
        f"user-data={q(user)}",
        ""]))
    # The 3DS's top screen: 400 x 240, a 60 degree vertical field of view (renderer.cpp kFovY)
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
                    time.sleep(0.3)        # written asynchronously
                    img = Image.open(new[-1]).convert("RGB")
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


def write_case(shots, name):
    """The test case: per shot the player behind the camera, the clock, then CAM + HUD:off + POSE, then SHOT."""
    lines = ["# start: Seyda Neen", "# Written by tools/test/sidebyside.py (side-by-side shots with OpenMW)",
             "0.5 0 0 0 0 FRAMECAP:off"]
    for s in shots:
        x, y, z, yaw, pitch = s["cam"]
        bx, by = x - math.sin(math.radians(yaw)) * 300, y - math.cos(math.radians(yaw)) * 300
        setup = []
        if "cell" in s:
            lines.append(f"0.5 0 0 0 0 GOTO:{s['cell'].replace(' ', '_')}")
            setup.append(f"TPIN:{fmt(bx)}:{fmt(by)}:{fmt(z)}:{fmt(yaw)}")
        else:
            setup.append(f"TP:{fmt(bx)}:{fmt(by)}:{fmt(z)}:{fmt(yaw)}")
        if "hour" in s:
            setup.append(f"HOUR:{s['hour']}")
        lines.append("1 0 0 0 0 " + "+".join(setup))
        view = [f"CAM:{fmt(x)}:{fmt(y)}:{fmt(z)}:{fmt(yaw)}:{fmt(pitch)}", "HUD:off"]
        if "actor" in s and "group" in s:
            pose = f"POSE:{s['actor'].replace(' ', '%')}:{s['group']}:{fmt(s['at'])}"
            if "face" in s:
                pose += f":{s['face']}"
            view.append(pose)
        lines.append("1 0 0 0 0 " + "+".join(view))
        lines.append("0.5 0 0 0 0 SHOT")
    case = ROOT / "tools" / "test" / "cases" / "scratch" / f"{name}.txt"
    case.parent.mkdir(parents=True, exist_ok=True)
    case.write_text("\n".join(lines) + "\n")
    return case


def run_3ds(shots, out, name, emu, data, exe):
    case = write_case(shots, name)
    raw = out / "3ds-raw"
    if raw.exists():
        shutil.rmtree(raw)
    cmd = ["powershell", "-NoProfile", "-File", str(ROOT / "tools" / "test" / "run-test.ps1"), f"scratch/{name}",
           "-Data", data, "-Start", "Seyda Neen", "-Shots", str(raw)]
    if emu:
        cmd += ["-Fast", "-Wait", "900"]
    else:
        if not exe:
            own = ROOT / "build" / "native-sbs" / "mw3ds-native.exe"
            exe = str(own if own.exists() else ROOT / "build" / "native" / "mw3ds-native.exe")
        cmd += ["-Native", "-Exe", exe, "-Sd", str(ROOT / "build" / "native-sbs" / "sd")]
    print("3ds:", case.relative_to(ROOT), "emulator" if emu else "native")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        if "RESULT" in ln or "FAIL" in ln or "holds" in ln:
            print("3ds:", ln.strip())
    pngs = sorted(raw.glob("shot_step_*.png"))
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
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--exe", default="", help="another native build")
    ap.add_argument("--timeout", type=float, default=300)
    a = ap.parse_args()
    shots = read_shots(a.shots)
    out = Path(a.out)
    if not a.pc_only:
        run_3ds(shots, out, "sbs-" + Path(a.shots).stem, a.emu, a.data, a.exe)
    if not a.ds_only:
        run_openmw(shots, out / "pc", a.timeout)
    compose(shots, out)


if __name__ == "__main__":
    main()
