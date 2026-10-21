"""Play fighting: a fighter set on the passive player in the 3DS engine and in OpenMW, the same spot, the same seeds, and
what each did compared: how it moved (the jitter report), when and how it struck or cast, what it did to the player.

    python tools/test/playfight.py <fights.txt> [--seeds 1-5] [--out build/playfight] [--pc-only] [--3ds-only]
                                   [--exe build/native/mw3ds-native.exe] [--data out/world]
                                   [--video] [--video-seed N]
    python tools/test/playfight.py --report <out>            (the report again from the traces already there)

A fights file has one fight per line (# starts a comment), a name then key=value pairs:

    guard   fighter="imperial guard"  at=-3514,-62273,520,180  dist=300  seconds=20  health=2000

    fighter=id   the NPC / creature (Morrowind.esm), made dist units ahead of the player, facing them, and sent at them
    at=x,y,z,yaw where the player stands (feet; degrees, yaw 0 looks along +Y) and does nothing
    seconds=s    how long the fight runs (game time)
    health=n     the player's Health and maximum: a passive target that outlasts the fight
    retreat=u    the player backs away from the fighter, facing it, at u units a second (0 to 125, a walk; default 0: stands
                 still). The 3DS gets u / 291 of the stick, OpenMW u / 125 of its walk, as both measured on flat ground
    wait=s       seconds after the 3DS's teleport for the cell and its people to load (default 1; a town: 4)
    ground=z     the floor's height there, for the default camera (0.7 m above it; default 181, Seyda Neen's road)
    follow=dx,dy,dz,yaw,pitch   a camera that follows the player: that far from their feet (world axes) looking that way
    cam=x,y,z,yaw,pitch   the camera for --video (left out: from the side, 450 units off, over the middle of the two)

--video also films the first seed (--video-seed) of each fight, 15 pictures a second, a static camera: the 3DS from SHOT
steps, OpenMW from a screen capture of its window (ffmpeg gdigrab; keep the window uncovered), joined 3DS left, OpenMW
right into <out>/<fight>/video.mp4. The two start together to within about half a second.

Each fight runs once per seed. The 3DS side is one native run of a generated case (SEED:<n>, HOSTILE, TRACE); OpenMW is
one process per seed (--random-seed <n>), its Lua mod in tools/test/playfight/ logging the same fields each frame:
`id time x y z yaw lowerGroup upperGroup health`. Rolls are not shared between the engines (their order differs): the
seeds make each side repeatable, and the report compares them over the seeds. Writes <out>/<fight>/<side>-<seed>.csv
(those lines) and <out>/report.txt:

  jitter   speed noise (RMS of the per-frame speed less the median of 9 frames, over the mean, while it runs), turn noise
           (RMS one-frame turn while it runs), heading reversals a second (a turn one way then back, over 0.005 rad),
           how near it came to the player
  fight    first blow's time, blows and casts a second, hits on the player (health drops), damage per hit, player dead
"""
import argparse
import math
import os
import re
import shutil
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sidebyside as sbs      # (OpenMW's folders, .env, the window and the log reading are shared)

MOD = Path(__file__).resolve().parent / "playfight"
STICK_RUN = 291.0     # the 3DS player at full stick: units a second (measured)
OPENMW_WALK = 125.0   # OpenMW's player walking backwards at full, no one hitting them: units a second (measured)
PF = Path(os.environ.get("MW3DS_PF_DIR") or ROOT / "build" / "openmw-pf")


def read_fights(path):
    fights = []
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        words = shlex.split(line)
        f = {"name": words[0], "dist": 300.0, "seconds": 15.0, "health": 2000.0, "retreat": 0.0, "wait": 1.0, "at": [-3514.0, -62273.0, 520.0, 180.0]}
        for w in words[1:]:
            if "=" not in w:
                sys.exit(f"{path}:{n}: '{w}' is not key=value")
            k, v = w.split("=", 1)
            if k == "at":
                f["at"] = [float(x) for x in v.split(",")]
                if len(f["at"]) != 4:
                    sys.exit(f"{path}:{n}: at= wants x,y,z,yaw")
            elif k in ("dist", "seconds", "health", "ground", "retreat", "wait"):
                f[k] = float(v)
            else:
                f[k] = v
        if "fighter" not in f:
            sys.exit(f"{path}:{n}: no fighter=")
        if isinstance(f.get("follow"), str):
            f["follow"] = [float(x) for x in f["follow"].split(",")]
            f["cam"] = f["follow"]
        elif isinstance(f.get("cam"), str):
            f["cam"] = [float(x) for x in f["cam"].split(",")]
        elif "cam" not in f:
            x, y, z, yaw = f["at"]
            r = math.radians(yaw)
            # (a player backing away travels retreat x seconds: the camera stands back to keep both in view,
            # over the middle of where they go)
            travel = f["retreat"] * f["seconds"]
            mx, my = x + math.sin(r) * (f["dist"] / 2 - travel / 2), y + math.cos(r) * (f["dist"] / 2 - travel / 2)
            off = 450 + travel * 0.9
            f["cam"] = [mx + math.cos(r) * off, my - math.sin(r) * off, float(f.get("ground", 181.0)) + 130.0, (yaw - 90.0) % 360.0, -6.0]
        fights.append(f)
    return fights


def read_seeds(text):
    seeds = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        seeds += list(range(int(a), int(b or a) + 1))
    return seeds


# ---- OpenMW

def write_openmw_setup(fight, seed):
    """build/openmw-pf: the config of sidebyside's, this mod, and fights.lua for one fight."""
    mw = Path(sbs.env_value("MW3DS_MORROWIND_DIR"))
    data_files = mw / "Data Files"
    data = PF / "data"
    if data.exists():
        shutil.rmtree(data)
    shutil.copytree(MOD, data)
    x, y, z, yaw = fight["at"]
    (data / "scripts" / "mw3ds_pf" / "fights.lua").write_text(
        "return {\n\t{ fighter = %s, px = %s, py = %s, pz = %s, pyaw = %r, dist = %s, seconds = %s, health = %s, retreat = %s,\n"
        "\t  cam = { x = %s, y = %s, z = %s, yaw = %r, pitch = %r, follow = %s } },\n}\n" % (
            sbs.lua_str(fight["fighter"].lower()), x, y, z, math.radians(yaw), fight["dist"], fight["seconds"], fight["health"], min(1.0, fight["retreat"] / OPENMW_WALK),
            fight["cam"][0], fight["cam"][1], fight["cam"][2], math.radians(fight["cam"][3]), math.radians(fight["cam"][4]),
            "true" if "follow" in fight else "false"))
    user = PF / "user"
    user.mkdir(parents=True, exist_ok=True)

    def q(p):
        return '"' + str(p).replace("\\", "/") + '"'
    install = (sbs.OPENMW / "openmw.cfg").read_text(errors="replace").splitlines()
    fallbacks = [ln for ln in install if ln.startswith("fallback=")]
    (PF / "openmw.cfg").write_text("\n".join([
        "# Written by tools/test/playfight.py",
        f"data={q(sbs.OPENMW / 'resources' / 'vfs-mw')}",
        f"data={q(data_files)}",
        f"data={q(data)}",
        "fallback-archive=Morrowind.bsa",
        "content=Morrowind.esm",
        "content=mw3ds_pf.omwscripts",
        f"user-data={q(user)}"] + fallbacks + [""]))
    ini = mw / "Morrowind.ini"
    importer = sbs.OPENMW / "openmw-iniimporter.exe"
    if ini.exists() and importer.exists():
        subprocess.run([str(importer), "-A", "-i", str(ini), "-c", str(PF / "openmw.cfg")], capture_output=True, text=True)
    (PF / "settings.cfg").write_text("\n".join([
        "# Written by tools/test/playfight.py",
        "[Video]", "resolution x = 400", "resolution y = 240", "window mode = 2", "window border = true",
        "vsync mode = 0", "framerate limit = 60", "",
        "[Camera]", "field of view = 60", "", "[Game]", "", ""]))
    return user


def find_ffmpeg():
    import sbsvideo
    return sbsvideo.find_ffmpeg()


def start_capture(proc, video):
    """ffmpeg filming OpenMW's window (its client area) at 15 pictures a second, scaled to 400 x 240; the process or None."""
    import win32gui
    hwnd = None
    for _ in range(40):
        hwnd = sbs.find_window(proc.pid)
        if hwnd:
            break
        time.sleep(0.5)
    if not hwnd:
        print("openmw: no window to film")
        return None
    win32gui.SetForegroundWindow(hwnd)
    x, y = win32gui.ClientToScreen(hwnd, (0, 0))
    r = win32gui.GetClientRect(hwnd)
    cmd = [find_ffmpeg(), "-v", "error", "-y", "-f", "gdigrab", "-framerate", "30", "-offset_x", str(x), "-offset_y", str(y),
           "-video_size", f"{r[2]}x{r[3]}", "-i", "desktop", "-vf", "scale=400:240,fps=15", "-c:v", "libx264", "-pix_fmt", "yuv420p",
           str(video)]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def run_openmw(fight, seed, dest, timeout, video=None):
    exe = sbs.OPENMW / "openmw.exe"
    if not exe.exists():
        sys.exit(f"playfight: no {exe}")
    user = write_openmw_setup(fight, seed)
    for f in (PF / "openmw.log", user / "openmw.log"):
        if f.exists():
            f.unlink()
    cmd = [str(exe), "--replace=config", f"--config={PF}", "--skip-menu", "--start=Seyda Neen", "--no-grab", "--no-sound",
           f"--random-seed={seed}"]
    proc = subprocess.Popen(cmd, cwd=sbs.OPENMW)
    lines, done, cap = [], False, None
    try:
        deadline = time.time() + timeout
        while time.time() < deadline and not done and proc.poll() is None:
            logs = [f for f in (PF / "openmw.log", user / "openmw.log") if f.exists()]
            if logs:
                lines = [ln.split("MW3DS-PF", 1)[1].strip() for ln in logs[0].read_text(errors="replace").splitlines() if "MW3DS-PF" in ln]
                done = any(ln == "done" for ln in lines)
                # (filming from the fight's start: the first line it logs)
                if video and not cap and any(ln.startswith("begin") for ln in lines):
                    cap = start_capture(proc, video)
            time.sleep(0.1 if video else 0.5)
    finally:
        if cap:
            try:
                cap.communicate(b"q", timeout=20)
            except subprocess.TimeoutExpired:
                cap.kill()
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    if not done:
        print(f"openmw: {fight['name']} seed {seed} did not finish")
    dest.write_text("\n".join(ln[len("trace "):] for ln in lines if ln.startswith("trace ")) + "\n")


# ---- The 3DS engine

def write_case(fights, seeds, name):
    """One case: per fight and seed the player put at `at` with their health, the fighter placed ahead and set on them."""
    lines = ["# start: Seyda Neen", "# Written by tools/test/playfight.py", "0.5 0 0 0 0 FRAMECAP:off+PCRACE:dark_elf+PCSEX:0"]
    for f in fights:
        x, y, z, yaw = f["at"]
        fid = f["fighter"].replace(" ", "%")
        for seed in seeds:
            lines += [f"1 0 0 0 0 TP:{sbs.fmt(x)}:{sbs.fmt(y)}:{sbs.fmt(z)}:{sbs.fmt(yaw)}+HEALTHMAX:{int(f['health'])}+SEED:{seed}",
                      f"{sbs.fmt(f['wait'])} 0 0 0",
                      f"0.3 0 0 0 0 HIDEACTORS:{sbs.HIDE_RADIUS}",
                      f"1 0 0 0 0 PLACE:{fid}:{sbs.fmt(f['dist'])}",
                      f"0.1 0 0 0 0 ENABLE:{fid}",
                      f"0.1 0 0 0 0 TRACE:{fid}+HOSTILE:{fid}",
                      f"{sbs.fmt(f['seconds'])} 0 {-f['retreat'] / STICK_RUN:.3f} 0 0",
                      f"0.1 0 0 0 0 TRACE:off+DISABLE:{fid}"]
    case = ROOT / "tools" / "test" / "cases" / "scratch" / f"{name}.txt"
    case.parent.mkdir(parents=True, exist_ok=True)
    case.write_text("\n".join(lines) + "\n")
    return case


def write_video_case(fight, seed, name):
    """One fight filmed: the steps of write_case, then a SHOT every 1/15 s through the fight."""
    x, y, z, yaw = fight["at"]
    cx, cy, cz, cyaw, cpitch = fight["cam"]
    cam_token = "CAMREL" if "follow" in fight else "CAM"
    cam_at = f"{cam_token}:{sbs.fmt(cx)}:{sbs.fmt(cy)}:{sbs.fmt(cz)}:{sbs.fmt(cyaw)}:{sbs.fmt(cpitch)}"
    fid = fight["fighter"].replace(" ", "%")
    lines = ["# start: Seyda Neen", "# Written by tools/test/playfight.py (--video)", "0.5 0 0 0 0 FRAMECAP:off+PCRACE:dark_elf+PCSEX:0",
             f"1 0 0 0 0 TP:{sbs.fmt(x)}:{sbs.fmt(y)}:{sbs.fmt(z)}:{sbs.fmt(yaw)}+HEALTHMAX:{int(fight['health'])}+SEED:{seed}",
             f"{sbs.fmt(fight['wait'])} 0 0 0",
             f"0.3 0 0 0 0 HIDEACTORS:{sbs.HIDE_RADIUS}",
             f"1 0 0 0 0 PLACE:{fid}:{sbs.fmt(fight['dist'])}",
             f"0.1 0 0 0 0 ENABLE:{fid}",
             f"0.5 0 0 0 0 {cam_at}+POV:third+HUD:off+VMPOSE:off",
             f"{1 / 15:.4f} 0 0 0 0 HOSTILE:{fid}+{cam_at}+SHOT"]
    lines += [f"{1 / 15:.4f} 0 {-fight['retreat'] / STICK_RUN:.3f} 0 0 {cam_at}+SHOT"] * (int(fight["seconds"] * 15) - 1)
    lines.append(f"0.1 0 0 0 0 DISABLE:{fid}")
    case = ROOT / "tools" / "test" / "cases" / "scratch" / f"{name}.txt"
    case.write_text("\n".join(lines) + "\n")
    return case


def film_3ds(fight, seed, name, exe, data, raw):
    """The 3DS side's pictures of one fight in raw/ (f0000.png ...), 400 x 240."""
    write_video_case(fight, seed, name)
    if raw.exists():
        shutil.rmtree(raw)
    cmd = [sys.executable, str(ROOT / "tools" / "test" / "run_test.py"), f"scratch/{name}", "--data", data, "--start", "Seyda Neen",
           "--native", "--exe", exe or str(ROOT / "build" / "native" / "mw3ds-native.exe"), "--sd",
           str((ROOT / "build" / "native-pf" / "sd").relative_to(ROOT)), "--shots", str(raw)]
    subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    pngs = sorted(raw.glob("shot_step_*.png"), key=lambda p: [int(t) for t in re.findall(r"\d+", p.stem)])
    from PIL import Image
    for i, png in enumerate(pngs):
        Image.open(png).convert("RGB").crop((0, 0, 400, 240)).save(raw / f"f{i:04d}.png")
    return len(pngs)


def join_video(raw, pc_video, dest):
    """The 3DS pictures left, OpenMW's film right."""
    cmd = [find_ffmpeg(), "-v", "error", "-y", "-framerate", "15", "-i", str(raw / "f%04d.png"), "-i", str(pc_video),
           "-filter_complex", "[0:v]pad=ceil(iw/2)*2:ceil(ih/2)*2[a];[a][1:v]hstack=inputs=2:shortest=1",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", str(dest)]
    return subprocess.call(cmd) == 0


def run_3ds(fights, seeds, out, name, exe, data):
    case = write_case(fights, seeds, name)
    sd = ROOT / "build" / "native-pf" / "sd"
    exe = exe or str(ROOT / "build" / "native" / "mw3ds-native.exe")
    cmd = [sys.executable, str(ROOT / "tools" / "test" / "run_test.py"), f"scratch/{name}", "--data", data,
           "--start", "Seyda Neen", "--native", "--exe", exe, "--sd", str(sd.relative_to(ROOT))]
    print("3ds:", case.relative_to(ROOT))
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        if "RESULT" in ln or "FAIL" in ln:
            print("3ds:", ln.strip())
    log = sd / "3ds" / "mw3ds" / "log.txt"
    if not log.exists():
        sys.exit(f"playfight: no {log}")
    runs, cur = [], None
    for ln in log.read_text(errors="replace").splitlines():
        if "trace: begin" in ln:
            cur = []
            runs.append(cur)
        elif "trace: " in ln and cur is not None:
            cur.append(ln.split("trace: ", 1)[1].strip())
    n = 0
    for f in fights:
        for seed in seeds:
            if n < len(runs):
                d = out / f["name"]
                d.mkdir(parents=True, exist_ok=True)
                (d / f"3ds-{seed}.csv").write_text("\n".join(runs[n]) + "\n")
            else:
                print(f"3ds: no trace for {f['name']} seed {seed}")
            n += 1


# ---- The report

def load(path):
    """Rows (id, t, x, y, z, yaw, lower, upper, health) of one csv. The 3DS has its one group in `lower` and the group's
    animation time (a number, no group) in `upper`."""
    rows = []
    for ln in Path(path).read_text().splitlines():
        p = ln.split()
        if len(p) >= 9:
            # (an id may have spaces: the last eight fields are time, x, y, z, yaw, two groups, health)
            try:
                rows.append((" ".join(p[:-8]), *[float(v) for v in p[-8:-3]], p[-3], p[-2], float(p[-1])))
            except ValueError:
                pass
    return rows


def classify(group):
    g = group.lower()
    if g in ("-", ""):
        return None
    if g.startswith(("attack", "weapon", "handtohand", "bowandarrow", "crossbow", "throwweapon")):
        return "blow"
    if g.startswith(("cast", "spellcast")):
        return "cast"
    if g.startswith(("hit", "knock")):
        return "hurt"
    return None


def analyse(rows):
    """Jitter and fight numbers of one trace."""
    me = [r for r in rows if r[0] != "player"]
    pl = [r for r in rows if r[0] == "player"]
    m = {}
    # jitter: the noise on steady motion. Per frame speed against the median of its 9 frames (dt from the stamps), over
    # the frames that run (above 20 a second); heading: reversals a second and the RMS one-frame turn while it runs
    v, yaw_d = [], []
    for a, b in zip(me, me[1:]):
        dt = b[1] - a[1]
        if dt <= 1e-4:
            v.append(None)
            yaw_d.append(None)
            continue
        v.append(math.hypot(b[2] - a[2], b[3] - a[3]) / dt)
        yaw_d.append(((b[5] - a[5] + math.pi) % (2 * math.pi) - math.pi, v[-1]))
    res, speeds = [], []
    for i in range(4, len(v) - 4):
        win = [x for x in v[i - 4:i + 5] if x is not None]
        if v[i] is None or len(win) < 9:
            continue
        med = statistics.median(win)
        if med > 20.0:
            res.append(v[i] - med)
            speeds.append(v[i])
    secs = me[-1][1] - me[0][1] if len(me) > 1 else 0.0
    m["moving"] = len(speeds)
    m["speed"] = statistics.mean(speeds) if speeds else 0.0
    m["speedjit"] = math.sqrt(statistics.mean(r * r for r in res)) / m["speed"] if speeds else 0.0
    turns = [d for d in yaw_d if d is not None and d[1] > 20.0]
    m["yawjit"] = math.sqrt(statistics.mean(d[0] ** 2 for d in turns)) if turns else 0.0
    ys = [d[0] for d in yaw_d if d is not None]
    rev = sum(1 for a, b in zip(ys, ys[1:]) if abs(a) > 0.005 and abs(b) > 0.005 and a * b < 0)
    m["yawrev"] = rev / secs if secs else 0.0
    near = [math.hypot(r[2] - p[2], r[3] - p[3]) for r, p in zip(me, pl)]
    m["closest"] = min(near) if near else float("nan")
    # fight: group entries (a change of group is one blow or cast), hits on the player
    last = {}
    blows, casts, first = 0, 0, None
    for r in me:
        for col in (6, 7):
            c = classify(r[col])
            if r[col] != last.get(col):
                last[col] = r[col]
                if c == "blow":
                    blows += 1
                    first = r[1] if first is None else first
                elif c == "cast":
                    casts += 1
                    first = r[1] if first is None else first
    m["firstact"] = first if first is not None else float("nan")
    m["blows"] = blows / secs if secs else 0.0
    m["casts"] = casts / secs if secs else 0.0
    hits, dmg = 0, []
    for a, b in zip(pl, pl[1:]):
        if b[8] < a[8] - 0.01:
            hits += 1
            dmg.append(a[8] - b[8])
    m["hits"] = hits
    m["dmg"] = statistics.mean(dmg) if dmg else 0.0
    m["dead"] = 1.0 if pl and pl[-1][8] <= 0 else 0.0
    m["secs"] = secs
    return m


COLS = [("moving", "running frames", "%.0f"), ("speed", "run speed", "%.0f"), ("speedjit", "speed noise (RMS/mean)", "%.3f"),
        ("yawjit", "turn noise rad/frame", "%.4f"), ("yawrev", "yaw reversals/s", "%.2f"), ("closest", "closest to player", "%.0f"),
        ("firstact", "first blow/cast s", "%.2f"), ("blows", "blows/s", "%.2f"),
        ("casts", "casts/s", "%.2f"), ("hits", "hits on player", "%.1f"), ("dmg", "damage/hit", "%.1f"), ("dead", "player dead", "%.2f")]


def report(out):
    lines = []
    for d in sorted(p for p in Path(out).iterdir() if p.is_dir()):
        sides = {}
        for side in ("3ds", "pc"):
            runs = [analyse(load(f)) for f in sorted(d.glob(f"{side}-*.csv")) if load(f)]
            if runs:
                sides[side] = runs
        if not sides:
            continue
        lines.append(f"== {d.name}  ({', '.join(f'{k}: {len(v)} seeds' for k, v in sides.items())})")
        lines.append(f"  {'':24s} {'3DS mean':>10s} {'sd':>7s} {'OpenMW mean':>12s} {'sd':>7s}")
        for key, label, fm in COLS:
            cells = []
            for side in ("3ds", "pc"):
                vals = [r[key] for r in sides.get(side, []) if not math.isnan(r[key])]
                cells += [fm % statistics.mean(vals), "%.2f" % (statistics.pstdev(vals) if len(vals) > 1 else 0.0)] if vals else ["-", "-"]
            lines.append(f"  {label:24s} {cells[0]:>10s} {cells[1]:>7s} {cells[2]:>12s} {cells[3]:>7s}")
    text = "\n".join(lines) + "\n"
    (Path(out) / "report.txt").write_text(text)
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fights", nargs="?")
    ap.add_argument("--seeds", default="1-3")
    ap.add_argument("--out", default=str(ROOT / "build" / "playfight"))
    ap.add_argument("--pc-only", action="store_true")
    ap.add_argument("--3ds-only", dest="ds_only", action="store_true")
    ap.add_argument("--exe", default="")
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--video", action="store_true", help="also film one seed of each fight, both sides, joined")
    ap.add_argument("--video-seed", type=int, default=0, help="the seed filmed (default: the first)")
    ap.add_argument("--report", metavar="OUT", help="only the report, from the traces in OUT")
    a = ap.parse_args()
    if a.report:
        report(a.report)
        return
    if not a.fights:
        ap.error("a fights file")
    fights = read_fights(a.fights)
    seeds = read_seeds(a.seeds)
    out = Path(a.out)
    if a.video:
        vseed = a.video_seed or seeds[0]
        for f in fights:
            d = out / f["name"]
            d.mkdir(parents=True, exist_ok=True)
            n = film_3ds(f, vseed, "pf-video-" + f["name"], a.exe, a.data, d / "video-3ds")
            print(f"3ds: {n} pictures of {f['name']}")
            print(f"openmw: filming {f['name']} seed {vseed}")
            run_openmw(f, vseed, d / "video-pc.csv", 60 + 2 * f["seconds"], video=d / "video-pc.mp4")
            ok = (d / "video-pc.mp4").exists() and join_video(d / "video-3ds", d / "video-pc.mp4", d / "video.mp4")
            print(f"video: {d / 'video.mp4'}" if ok else f"video: {f['name']} failed")
        return
    if not a.pc_only:
        run_3ds(fights, seeds, out, "pf-" + Path(a.fights).stem, a.exe, a.data)
    if not a.ds_only:
        for f in fights:
            (out / f["name"]).mkdir(parents=True, exist_ok=True)
            for seed in seeds:
                print(f"openmw: {f['name']} seed {seed}")
                run_openmw(f, seed, out / f["name"] / f"pc-{seed}.csv", 60 + 2 * f["seconds"])
    report(out)


if __name__ == "__main__":
    main()
