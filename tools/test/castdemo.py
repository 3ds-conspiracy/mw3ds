"""Cast demo: OpenMW's side of a spell cast at a person with a guard about (tools/test/castdemo/ is its Lua mod).

    python tools/test/castdemo.py [--seed 1] [--out build/castdemo] [--video <3ds save folder>]

OpenMW 0.51 (as sidebyside.py runs it, Morrowind.esm only) starts in Seyda Neen, the player goes to Pelagiad and casts a made
spell, Frost Damage 12 - 15 for 10 s on Target (a reporter's "Grand Frostbite", cost 55), at Dralas Gilu from 250 units, skills
and Magicka high so the cast succeeds. Every quarter second the log gets his health, the bounty and each guard within 3000
units (distance); a guard's AI package is logged whenever it changes. Writes <out>/castdemo-<seed>.txt (those lines) and prints
a summary: when he took damage, when he died, the bounty, what the guards did.

--video <folder with test_issue61.sav> films both sides side by side (3DS left, OpenMW right), first person, 15 pictures a
second, from the moment of the cast: OpenMW from a screen capture of its window (ffmpeg gdigrab: keep the window uncovered), the
3DS from a native replay of the reporter's save (the same spell cast at Dralas Gilu, SHOT steps). No sound.
"""
import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sidebyside as sbs

ROOT = Path(__file__).resolve().parents[2]
MOD = Path(__file__).resolve().parent / "castdemo"
CD = ROOT / "build" / "openmw-cd"


def setup():
    mw = Path(sbs.env_value("MW3DS_MORROWIND_DIR"))
    data_files = mw / "Data Files"
    data = CD / "data"
    if data.exists():
        shutil.rmtree(data)
    shutil.copytree(MOD, data)
    user = CD / "user"
    user.mkdir(parents=True, exist_ok=True)

    def q(p):
        return '"' + str(p).replace("\\", "/") + '"'
    install = (sbs.OPENMW / "openmw.cfg").read_text(errors="replace").splitlines()
    fallbacks = [ln for ln in install if ln.startswith("fallback=")]
    (CD / "openmw.cfg").write_text("\n".join([
        "# Written by tools/test/castdemo.py",
        f"data={q(sbs.OPENMW / 'resources' / 'vfs-mw')}", f"data={q(data_files)}", f"data={q(data)}",
        "fallback-archive=Morrowind.bsa", "content=Morrowind.esm", "content=mw3ds_cd.omwscripts", f"user-data={q(user)}"]
        + fallbacks + [""]))
    ini, importer = mw / "Morrowind.ini", sbs.OPENMW / "openmw-iniimporter.exe"
    if ini.exists() and importer.exists():
        subprocess.run([str(importer), "-A", "-i", str(ini), "-c", str(CD / "openmw.cfg")], capture_output=True, text=True)
    (CD / "settings.cfg").write_text("\n".join([
        "[Video]", "resolution x = 400", "resolution y = 240", "window mode = 2", "vsync mode = 0", "framerate limit = 60", "",
        "[Camera]", "field of view = 60", "", "[Game]", "", ""]))
    return user


def film_3ds(saves, exe, dest, seconds):
    """The 3DS side: the reporter's save, the same spell at Dralas Gilu, recorded by tools/test/video.py (a fixed 1/30 s a frame, 15
    pictures a second): the top screen, from the cast on, `seconds` long, with the game's sound. Returns the cast's time in the
    recording (the steps before it; a SHOT step is not a fixed time: it ends on its picture, so those videos ran twice too fast)."""
    before = ["# start: save:sdmc:/3ds/mw3ds/test_issue61.sav", "3 0 0 0 0 MSG", "0.3 0 0 0 0 SEED:7",
              "0.3 0 0 0 0 SETSKILL:destruction:100", "0.3 0 0 0 0 SETATTR:willpower:100", "0.3 0 0 0 0 SETATTR:luck:100",
              "0.3 0 0 0 0 SETMAGICKA:1000", "0.3 0 0 0 0 TP:1519:-57400:1500:180+POV:first+HUD:off", "8 0 0 0",
              "0.3 0 0 0 0 FACE:dralas%gilu", "1 0 0 0"]
    lead = sum(float(ln.split()[0]) for ln in before[1:])
    steps = before + ["0.1 0 0 0 0 CAST:mw3ds_spell_962433", f"{seconds + 2} 0 0 0"]
    case = ROOT / "tools" / "test" / "cases" / "scratch" / "castdemo-3ds.txt"
    case.parent.mkdir(parents=True, exist_ok=True)
    case.write_text(chr(10).join(steps) + chr(10))
    cmd = [sys.executable, str(ROOT / "tools" / "test" / "video.py"), "scratch/castdemo-3ds", "--every", "2", "--out", str(dest),
           "--exe", exe, "--sd", "build/native-pf/sd", "--saves", str(saves)]
    subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=900)
    return lead


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default=str(ROOT / "build" / "castdemo"))
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--video", default="", help="a folder with test_issue61.sav: film both sides side by side")
    ap.add_argument("--exe", default=str(ROOT / "build" / "native" / "mw3ds-native.exe"))
    a = ap.parse_args()
    user = setup()
    for f in (CD / "openmw.log", user / "openmw.log"):
        if f.exists():
            f.unlink()
    exe = sbs.OPENMW / "openmw.exe"
    proc = subprocess.Popen([str(exe), "--replace=config", f"--config={CD}", "--skip-menu", "--start=Seyda Neen", "--no-grab",
                             "--no-sound", f"--random-seed={a.seed}"], cwd=sbs.OPENMW)
    lines, done, cap, cap_t = [], False, None, 0.0
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    pcvideo = out / "castdemo-openmw.mp4"
    try:
        end = time.time() + a.timeout
        while time.time() < end and not done and proc.poll() is None:
            logs = [f for f in (CD / "openmw.log", user / "openmw.log") if f.exists()]
            if logs:
                lines = [ln.split("MW3DS-CD", 1)[1].strip() for ln in logs[0].read_text(errors="replace").splitlines() if "MW3DS-CD" in ln]
                done = any(ln == "done" for ln in lines)
                # (filming from the "begin" line, a second before the Use press, so the film is running when it comes)
                if a.video and not cap and any(ln.startswith("begin") for ln in lines):
                    import playfight
                    cap = playfight.start_capture(proc, pcvideo)
                    cap_t = time.time()
            time.sleep(0.1 if a.video else 0.5)
    finally:
        if cap:
            try:
                cap.communicate(b"q", timeout=20)
            except subprocess.TimeoutExpired:
                cap.kill()
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    (out / f"castdemo-{a.seed}.txt").write_text("\n".join(lines) + "\n")
    rows = [ln.split() for ln in lines if re.match(r"^\d+\.\d+ ", ln)]
    print("finished" if done else "did not finish (timeout)", flush=True)
    for ln in lines:
        if not re.match(r"^\d+\.\d+ ", ln) and ln != "done":
            print(" ", ln)
    if rows:
        h0 = float(rows[0][1])
        first = next((r for r in rows if float(r[1]) < h0 - 0.5), None)
        died = next((r for r in rows if float(r[1]) <= 0), None)
        print(f"  Dralas health at the cast {h0:.0f}; first drop at {first[0] + ' s (' + first[1] + ')' if first else 'never'}; "
              f"dead at {died[0] + ' s' if died else 'not within the run'}")
        bounties = sorted({r[2] for r in rows}, key=int)
        print(f"  bounty over the run: {', '.join(bounties)}")
        for r in rows[:: max(1, len(rows) // 14)]:
            print("   ", " ".join(r))
    if a.video and pcvideo.exists():
        # when the Use press came, on the capture's clock: the log's own time stamp of the "cast" line, less when ffmpeg started
        # (and about 0.3 s it takes to begin its first picture)
        logtxt = next((f for f in (CD / "openmw.log", user / "openmw.log") if f.exists()), None)
        cast_wall = None
        if logtxt:
            for ln in logtxt.read_text(errors="replace").splitlines():
                if "MW3DS-CD cast" in ln:
                    m = re.match(r"\[(\d+):(\d+):(\d+)\.(\d+)", ln)
                    if m:
                        now = time.localtime(cap_t)
                        cast_wall = time.mktime((now.tm_year, now.tm_mon, now.tm_mday, int(m[1]), int(m[2]), int(m[3]), 0, 0, -1)) + int(m[4]) / 1000.0
        pc_lead = max(0.0, (cast_wall - cap_t - 0.3) if cast_wall else 1.0)
        print(f"openmw: the cast comes {pc_lead:.2f} s into its film", flush=True)
        v3 = out / "castdemo-3ds.mp4"
        lead = film_3ds(Path(a.video), a.exe, v3, 20)
        print(f"3ds: cast at {lead:.1f} s of its film", flush=True)
        ok = join(v3, lead, pcvideo, pc_lead, out / "castdemo-sbs.mp4", 20)
        print(f"video: {out / 'castdemo-sbs.mp4'}" if ok else "video failed")


def join(v3, lead3, pcvideo, lead_pc, dest, seconds):
    """3DS (its top screen, from the cast) left, OpenMW (from the cast) right, `seconds` long, the 3DS side's sound."""
    import playfight
    cmd = [playfight.find_ffmpeg(), "-v", "error", "-y", "-ss", f"{lead3:.2f}", "-t", str(seconds), "-i", str(v3), "-ss", f"{lead_pc:.2f}", "-t",
           str(seconds), "-i", str(pcvideo), "-filter_complex",
           "[0:v]crop=400:240:0:0,fps=15[a];[1:v]fps=15[b];[a][b]hstack=inputs=2:shortest=1[v]", "-map", "[v]", "-map", "0:a?",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", str(dest)]
    return subprocess.call(cmd) == 0


if __name__ == "__main__":
    main()
