"""Runs the side-quest sweep batches (tools/test/questrun.py) one after another on the island data in Azahar:
    python tools/test/qrun_all.py [--from 1] [--to 19] [--prefix qrun]    results -> build/qrun-results.txt, logs -> build/qrun-logs
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
import run_test

ROOT = mwtools.ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="first", type=int, default=1)
    ap.add_argument("--to", type=int, default=19)
    ap.add_argument("--prefix", default="qrun")
    a = ap.parse_args()

    logs = ROOT / "build" / "qrun-logs"
    logs.mkdir(parents=True, exist_ok=True)
    # the binary as it is now: rebuilding during the sweep must not change what the batches run
    app = ROOT / "build" / "qrun-app.3dsx"
    shutil.copy2(ROOT / "mw3ds.3dsx", app)
    emu = Path(mwtools.get_env("MW3DS_EMU_DIR", "the Azahar folder"))
    log = emu / "user" / "sdmc" / "3ds" / "mw3ds" / "log.txt"
    for k in range(a.first, a.to + 1):
        name = f"{a.prefix}-{k}"
        run_test.run(name, data="out/world", start="Seyda Neen", wait=2700, pattern="zzzz", app=str(app))
        if log.exists():            # (an emulator that died at boot wrote none: the check below then counts nothing)
            shutil.copy2(log, logs / f"{name}.log")
        emu_log = emu / "user" / "log" / "azahar_log.txt"
        if emu_log.exists():
            shutil.copy2(emu_log, logs / f"{name}.emu.log")
        r = subprocess.run([sys.executable, str(mwtools.TEST_DIR / "questrun.py"), "--check", name],
                           capture_output=True, text=True, errors="replace")
        with open(ROOT / "build" / f"{a.prefix}-results.txt", "a", encoding="utf-8") as f:
            f.write(r.stdout)


if __name__ == "__main__":
    main()
