"""Runs story tests one after another in Azahar and prints each verdict (the main quest chapters by default):
    python tools/test/run_suite.py [--tests mq-ch1 mq-ch2] [--data out/world] [--start 'Seyda Neen'] [--chained]
                                   [--emu <azahar folder>] [--app <3dsx copy>] [--logs <folder>]
                                   [--resume-from <chain-mq-chN.sav>]
(--chained also keeps each chapter's closing save beside its log: <logs>/chain-mq-chN.sav. --resume-from starts the
first chapter from such a save, e.g. --chained --tests mq-ch10 mq-ch11 --resume-from <logs>/chain-mq-ch9.sav)
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
import run_test

ROOT = mwtools.ROOT


def chain_cases(tests, resume):
    """Writes chain-<test> cases: each chapter starts from the save the one before it ended with (its own setup then
    only raising what's behind: CHAIN), so the hand-offs between chapters get played rather than warped."""
    prev = "resume" if resume else None
    names = []
    for t in tests:
        lines = mwtools.read_lines(mwtools.CASES / f"{t}.txt")
        out = []
        if prev:
            # god mode (kept through the load) before the save comes in: a chapter that ended mid-fight (Ald
            # Daedroth's golden saint) would otherwise kill the player in the seconds before its own GOD step
            out.append("3 0 0 0 0 MSG")
            if any(re.match(r"^\S+ 0 0 0 0 GOD$", ln) for ln in lines):
                out.append("0.3 0 0 0 0 GOD")
            out += ["0.5 0 0 0 0 LOAD:chain", "4 0 0 0", "0.3 0 0 0 0 CHAIN"]
            lines = lines[1:]
        out += lines
        out += ["1 0 0 0 0 SAVE:chain", "1 0 0 0"]
        name = f"chain-{t}"
        mwtools.write_lines(mwtools.CASES / f"{name}.txt", out)
        names.append(name)
        prev = t
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tests", nargs="*", default=[])
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--start", default="Seyda Neen")
    ap.add_argument("--wait", type=int, default=2400)
    ap.add_argument("--chained", action="store_true")
    ap.add_argument("--emu", default="")
    ap.add_argument("--app", default="")
    ap.add_argument("--logs", default="")
    ap.add_argument("--resume-from", default="")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--old3ds", action="store_true")
    a = ap.parse_args()

    emu = a.emu or mwtools.get_env("MW3DS_EMU_DIR", "the Azahar folder")
    chain_save = Path(emu) / "user" / "sdmc" / "3ds" / "mw3ds" / "test_chain.sav"
    tests = [t for part in a.tests for t in part.split(",") if t]
    if not tests:
        tests = sorted((p.stem for p in mwtools.CASES.glob("mq-ch*.txt")), key=lambda n: int(re.sub(r"\D", "", n) or 0))
    if a.chained:
        if a.resume_from:
            chain_save.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(a.resume_from, chain_save)
        tests = chain_cases(tests, a.resume_from)
    logs = Path(a.logs) if a.logs else ROOT / "build" / "suite-logs"
    logs.mkdir(parents=True, exist_ok=True)
    log = Path(emu) / "user" / "sdmc" / "3ds" / "mw3ds" / "log.txt"
    for t in tests:
        out = run_test.run(t, start=a.start, data=a.data, wait=a.wait, emu=emu, app=a.app, fast=a.fast, old3ds=a.old3ds)
        shutil.copy2(log, logs / f"{t}.log")
        if a.chained and chain_save.exists():
            shutil.copy2(chain_save, logs / f"{t}.sav")
        verdict = next((ln for ln in out if ln.startswith("RESULT:")), "")
        print(f"{t:<10} {verdict}", flush=True)
        for ln in out:
            if re.search(r"expect: FAIL|drive: FAIL|monitor:|emulator faults", ln, re.IGNORECASE):
                print(f"           {ln}", flush=True)


if __name__ == "__main__":
    main()
