"""Runs a scripted test from tools/test/cases in Azahar (or natively) and prints its result lines (check: / journal / crime ...).
    python tools/test/run_test.py fg-rathunt                  (starts in Balmora, the Balmora-area data)
    python tools/test/run_test.py tg-bragor --data out/world  (the whole island)
    python tools/test/run_test.py mainquest --start "Seyda Neen"
    python tools/test/run_test.py ui-g1-g9 --native --shots build/shots/ui   (each SHOT step saved there as a PNG of both screens)
    python tools/test/run_test.py perf-balmora --old3ds       (Azahar as an Old 3DS; natively, the game takes its Old 3DS profile)
--saves <folder>: keep test_*.sav checkpoints there between native runs. --exe: another native build (build.py with
MW3DS_NATIVE_BUILD). --native draws in software (NATIVE_DRAW=1) only when the case has a SHOT step, or with --draw.
The last line is the verdict, "RESULT: PASS|FAIL (...)".
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
import run_emu

ROOT = mwtools.ROOT
DEFAULT_PATTERN = (r"check:|not offered|failed|expelled|crime:|expect:|monitor:|drive: (FAIL|stuck|crosshair miss)"
                   r"|was fighting|script error")
SHOT_STEP = re.compile(r"(^|[\s+])SHOT($|[\s+])")
QUIT_STEP = re.compile(r"(^|[\s+])QUIT($|[\s+])")
FAULT = re.compile(r"Unmapped ?\w+ @ \S+ at PC (0x[0-9A-Fa-f]+)|Unreachable code", re.IGNORECASE)


def count(lines, pattern):
    rx = re.compile(pattern, re.IGNORECASE)
    return sum(1 for ln in lines if rx.search(ln))


def run(test, start="Balmora", data="out/data", wait=300, app="", emu="", fast=False, native=False, sd="", exe="",
        saves="", draw=False, shots="", old3ds=False, pattern=DEFAULT_PATTERN, stereo=False):
    """Runs one case; returns the lines to print (the verdict last)."""
    mwtools.load_env()
    out = []
    if not native and not emu:
        emu = mwtools.get_env("MW3DS_EMU_DIR", "the Azahar folder")
    inputs = mwtools.read_lines(mwtools.CASES / f"{test}.txt")
    # A case that needs its own start cell (character creation on the prison ship) says so on its first line,
    # "# start: <cell>"; the game skips lines that are not steps
    if inputs:
        m = re.match(r"^#\s*start:\s*(.+?)\s*$", inputs[0])
        if m:
            start = m.group(1)
    # "# console: old" in its first lines: a case for the Old 3DS profile alone (run_native_all leaves it out of New 3DS runs)
    if any(re.match(r"^#\s*console:\s*old\s*$", ln) for ln in inputs[:5]):
        old3ds = True
    if native:
        # The game built for the PC (tools/test/native/build.py): same case file, same log lines, no emulator
        # --sd: its own SD folder, so runs can go in parallel. --sd, --saves and --exe are relative to the repo, as the
        # native exe (which runs there) sees them
        sd_dir = (ROOT / sd).resolve() if sd else ROOT / "build" / "native" / "sd"
        saves = str((ROOT / saves).resolve()) if saves else ""
        sd_app = sd_dir / "3ds" / "mw3ds"
        # A clean SD folder every run: saves and settings the case before left behind change what the next one starts in
        shutil.rmtree(sd_app, ignore_errors=True)
        sd_app.mkdir(parents=True, exist_ok=True)
        # --saves <folder>: the uber tests' checkpoints (SAVE:name / LOAD:name) kept across runs, so a long case resumes
        if saves:
            Path(saves).mkdir(parents=True, exist_ok=True)
            for f in Path(saves).glob("test_*"):
                shutil.copy2(f, sd_app)
        mwtools.write_lines(sd_app / "autoinput.txt", inputs)
        mwtools.write_lines(sd_app / "start.txt", [start])
        env = dict(os.environ)
        env["NATIVE_SD"] = str(sd_dir)
        env["MW3DS_DATA"] = str(ROOT / data)
        # --old3ds: the game is told it runs on an Old 3DS (its O3DS profile: view, budgets, frame cap)
        env["NATIVE_OLD3DS"] = "1" if old3ds else "0"
        # The software GPU (tools/test/native/native_gpu.cpp) only when something will be looked at: it is slower
        # (a MAPCELL step saves a picture too: the cell from above, tools/convert/maprender.py)
        env["NATIVE_DRAW"] = "1" if draw or any(SHOT_STEP.search(ln) or "MAPCELL:" in ln for ln in inputs) else "0"
        exe_path = str((ROOT / exe).resolve()) if exe else str(ROOT / mwtools.NATIVE_EXE)
        proc = subprocess.Popen([exe_path], cwd=ROOT, env=env, creationflags=mwtools.hidden_flags(),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            mwtools.kill_tree(proc)
        if saves:
            for f in sd_app.glob("test_*"):
                shutil.copy2(f, saves)
        log = sd_app / "log.txt"
        shot_dir = sd_app
    else:
        run_emu.run(inputs=inputs, wait=wait, start=start, data=data, app=app, emu=emu, fast=fast, old3ds=old3ds, stereo=stereo)
        log = Path(emu) / "user" / "sdmc" / "3ds" / "mw3ds" / "log.txt"
        shot_dir = log.parent

    lines = mwtools.read_lines(log) if log.exists() else []
    out.append(f"== {test}")
    rx = re.compile(pattern, re.IGNORECASE)
    out += [ln for ln in lines if rx.search(ln)]
    ended = any("autoinput: end" in ln.lower() for ln in lines)
    if not ended:
        out.append("(did not reach the end)")
    # Memory faults the emulator saw (reads / writes of unmapped memory: a crash on hardware), by code address
    faults = []
    if not native:
        emu_log = Path(emu) / "user" / "log" / "azahar_log.txt"
        # ("unmapped Read32 @ 0x0000000C at PC 0x002AA464" in today's Azahar, "UnmappedRead32 @ ..." before: the space is optional)
        if emu_log.exists():
            faults = [FAULT.search(ln) for ln in mwtools.read_lines(emu_log)]
            faults = [m for m in faults if m]
    if faults:
        pcs = Counter(m.group(1) if m.group(1) else "gpu-unreachable" for m in faults)
        top = sorted(pcs.items(), key=lambda kv: -kv[1])[:5]
        out.append("emulator faults: " + ", ".join(f"{k} x{v}" for k, v in top) + "  (python tools/build/addr2line.py <pc>)")

    # Verdict: story (expectations, driver failures, monitors, faults, reaching the end) and, apart, navigation
    # (the driver warped past a spot it couldn't walk: the story still ran, the route needs a look)
    passed = count(lines, "expect: PASS")
    failed = count(lines, "expect: FAIL")
    drive = count(lines, "drive: FAIL")
    warps = count(lines, r"drive: stuck at .*warped")
    misses = count(lines, "drive: crosshair miss")
    mon = count(lines, "monitor: ")
    # A case ending in QUIT must leave through the game's exit path to its last line (a crash on the way out left the
    # console on a black screen): the exit line and any crash line are shown with the result
    quit_case = any(QUIT_STEP.search(ln) for ln in inputs)
    shut_down = (not quit_case) or any("exit: shut down" in ln.lower() for ln in lines)
    if quit_case:
        out += [ln for ln in lines if re.search("exit: shut down|crash: ", ln, re.IGNORECASE)]
    bad = failed + drive + mon + (1 if faults else 0) + (0 if ended else 1) + (0 if shut_down else 1)
    out.append("RESULT: " + ("PASS" if bad == 0 else "FAIL")
               + f" ({passed} expectations met, {failed} failed, {drive} driver failures, {mon} monitor warnings"
               + (", emulator faults" if faults else "") + ("" if ended else ", did not finish")
               + ("" if shut_down else ", did not shut down") + ")"
               + (f"; {warps} navigation warps" if warps else "")
               + (f"; {misses} crosshair misses (activated directly)" if misses else ""))
    # SHOT steps as PNG (400 x 480: the top screen above the bottom one)
    if shots:
        subprocess.run([sys.executable, str(mwtools.TEST_DIR / "shots.py"), str(shot_dir),
                        shots if os.path.isabs(shots) else str(ROOT / shots)])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("test")
    ap.add_argument("--start", default="Balmora")
    ap.add_argument("--data", default="out/data")
    ap.add_argument("--wait", type=int, default=300)
    ap.add_argument("--app", default="")
    ap.add_argument("--emu", default="")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--native", action="store_true")
    ap.add_argument("--sd", default="")
    ap.add_argument("--exe", default="")
    ap.add_argument("--saves", default="")
    ap.add_argument("--draw", action="store_true")
    ap.add_argument("--shots", default="")
    ap.add_argument("--old3ds", action="store_true")
    ap.add_argument("--stereo", action="store_true", help="Azahar with the 3D slider at full (both eyes drawn)")
    ap.add_argument("--pattern", default=DEFAULT_PATTERN)
    a = ap.parse_args()
    for ln in run(a.test, a.start, a.data, a.wait, a.app, a.emu, a.fast, a.native, a.sd, a.exe, a.saves, a.draw,
                  a.shots, a.old3ds, a.pattern, a.stereo):
        print(ln, flush=True)


if __name__ == "__main__":
    main()
