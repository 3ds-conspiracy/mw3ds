"""Runs mw3ds.3dsx in Azahar and captures what it draws, via the app's own screenshots.
    python tools/test/run_emu.py                 one shot (both screens) after 3 s -> build/emu-shot.png
    python tools/test/run_emu.py --cams spawn "x y z yaw pitch" ...
                                                 one top-screen shot per pose, tiled 2-wide -> build/emu-cams.png
                                                 ("@Cell name" switches cell for the poses after it)
Converted game data in out/data (or --data <folder>) is linked onto the emulated SD card first (no copy).
  --start "Seyda Neen"   start in that cell, skipping character creation
  --emu <folder>         another Azahar copy (its own SD card and log): a spot check beside a sweep; only that
                         copy's emulator is stopped
  --fast                 no speed cap, no vsync (the ini is put back afterwards)
  --old3ds               the emulator runs as an Old 3DS (is_new_3ds = false: 64 MB, no speedup; the ini is put back
                         afterwards). It checks the memory map and behaviour, not 268 MHz frame times
Other scripts call run() with scripted `inputs` (lines of a test case).
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
QUIT_STEP = re.compile(r"(^|[\s+])QUIT($|[\s+])")


def _set_ini(ini, pairs):
    """Changes settings in Azahar's qt-config.ini; returns the original bytes to put back (or None)."""
    if not pairs or not ini.exists():
        return None
    original = ini.read_bytes()
    text = original.decode("utf-8", errors="surrogateescape")
    for key, value in pairs:
        text = text.replace(f"{key}\\default=true", f"{key}\\default=false")
        text = re.sub(rf"(?m)^{re.escape(key)}=[^\r\n]*", f"{key}={value}", text)
    ini.write_bytes(text.encode("utf-8", errors="surrogateescape"))
    return original


def _start_emulator(emu_dir, app):
    kw = {}
    if mwtools.IS_WINDOWS:
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 6          # SW_MINIMIZE
        kw["startupinfo"] = si
    return subprocess.Popen([str(mwtools.emulator_exe(emu_dir)), str(app)], **kw)


def _montage(shots, out):
    """The top screens of the given BMPs tiled two wide."""
    cols = min(2, len(shots))
    rows = (len(shots) + 1) // 2
    grid = Image.new("RGB", (400 * cols, 240 * rows))
    for i, p in enumerate(shots):
        with Image.open(p) as img:
            grid.paste(img.convert("RGB").crop((0, 0, 400, 240)), (400 * (i % 2), 240 * (i // 2)))
    grid.save(out)


def run(cams=None, inputs=None, wait=60, keep=False, start=None, data="out/data", app="", emu="", fast=False, old3ds=False):
    """Runs one emulator session; returns the lines it printed."""
    emu_dir = Path(emu or mwtools.get_env("MW3DS_EMU_DIR", "the Azahar folder"))
    app_path = Path(app) if app else ROOT / "mw3ds.3dsx"
    sd = emu_dir / "user" / "sdmc" / "3ds" / "mw3ds"
    log = sd / "log.txt"
    data_dir = ROOT / data
    out_lines = []
    (ROOT / "build").mkdir(exist_ok=True)

    mwtools.kill_processes_under(emu_dir, {"azahar.exe", "azahar"})
    sd.mkdir(parents=True, exist_ok=True)
    for p in [log, sd / "autocam.txt", sd / "autoshot", *sd.glob("shot*.bmp")]:
        p.unlink(missing_ok=True)
    # The game data on the emulated SD card: every copy links to the folder itself (a directory link: no second
    # 1 GB copy; the game only reads it). So don't rebuild that folder while an emulator runs.
    if data_dir.exists():
        mwtools.link_data_dir(sd / "data", data_dir)

    # --fast: the emulator's speed cap and vsync off for this run; --old3ds: an Old 3DS (the ini is restored after the run)
    pairs = []
    if fast:
        pairs += [("use_vsync", "false"), ("frame_limit", "900")]
    if old3ds:
        pairs += [("is_new_3ds", "false")]
    ini = emu_dir / "user" / "config" / "qt-config.ini"
    ini_backup = _set_ini(ini, pairs)
    proc = None
    try:
        (sd / "autoinput.txt").unlink(missing_ok=True)
        start_file = sd / "start.txt"
        if start:
            mwtools.write_lines(start_file, [start])
        else:
            start_file.unlink(missing_ok=True)
        if cams:
            mwtools.write_lines(sd / "autocam.txt", cams)
            done_pattern = "autocam done"
        elif inputs:
            # Scripted stick input "seconds moveX moveY lookX [jump]"; ends with a shot.bmp
            mwtools.write_lines(sd / "autoinput.txt", inputs)
            done_pattern = "autoshot"
            # A case ending in QUIT leaves through the game's exit path after its shot: the run is over when that has
            # written its last line (or the game stops writing: the stall below)
            if any(QUIT_STEP.search(ln) for ln in inputs):
                done_pattern = "exit: shut down"
        else:
            (sd / "autoshot").touch()
            done_pattern = "autoshot"

        proc = _start_emulator(emu_dir, app_path)
        rx = re.compile(done_pattern, re.IGNORECASE)
        deadline = time.time() + wait
        done = stalled = False
        offset = 0
        carry = ""

        def seen():
            """Does the log (read from where it was last read) now hold the done pattern?"""
            nonlocal offset, carry
            if not log.exists():
                return False
            with open(log, "rb") as f:
                f.seek(offset)
                chunk = f.read()
            offset += len(chunk)
            text = carry + chunk.decode("utf-8", errors="replace")
            carry = text[-200:]
            return bool(rx.search(text))

        while time.time() < deadline and not done:
            done = seen()
            if done:
                break
            time.sleep(0.5)
            # Give up early when the emulator is gone or the game stopped writing its log (a hang)
            if proc.poll() is not None:
                # (the emulator closes with a game that quit: done when the log says so)
                done = seen()
                stalled = not done
                break
            if log.exists() and time.time() - log.stat().st_mtime > 180:
                stalled = True
                break
        if stalled:
            out_lines.append("stalled: emulator exited or log quiet for 3 min")

        shots = sorted(sd.glob("shot_*.bmp"), key=lambda p: p.name)
        if done and shots:
            out = ROOT / "build" / "emu-cams.png"
            _montage(shots, out)
            out_lines.append(f"screenshot grid ({len(shots)}): {out}")
        elif done and (sd / "shot.bmp").exists():
            out = ROOT / "build" / "emu-shot.png"
            with Image.open(sd / "shot.bmp") as img:
                img.save(out)
            out_lines.append(f"screenshot: {out}")
        else:
            out_lines.append(f"no screenshot after {wait} s")

        if log.exists():
            lines = mwtools.read_lines(log)
            out_lines += [ln for ln in lines if not re.match(r"^\[\s*\d+\] fps", ln)][-40:]
            fps = [ln for ln in lines if re.search(r"\] fps", ln)]
            if fps:
                out_lines.append(fps[-1])
        else:
            out_lines.append("no app log")
        for p in [sd / "autoshot", sd / "autocam.txt", sd / "autoinput.txt"]:
            p.unlink(missing_ok=True)
    finally:
        if proc is not None and not keep:
            mwtools.kill_tree(proc)
        # --fast / --old3ds: the ini goes back even if the run failed
        if ini_backup is not None:
            time.sleep(0.5)
            ini.write_bytes(ini_backup)
    return out_lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cams", nargs="*")
    ap.add_argument("--wait", type=int, default=60)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--start")
    ap.add_argument("--data", default="out/data")
    ap.add_argument("--app", default="")
    ap.add_argument("--emu", default="")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--old3ds", action="store_true")
    a = ap.parse_args()
    for ln in run(a.cams, None, a.wait, a.keep, a.start, a.data, a.app, a.emu, a.fast, a.old3ds):
        print(ln)


if __name__ == "__main__":
    main()
