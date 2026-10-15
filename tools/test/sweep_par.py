"""Runs sweep batches (and tests) across several Azahar copies at once: one worker thread per emulator, each taking the
next unclaimed item until none are left. Each batch's check goes to build/par/<name>.result; they're appended to
build/<prefix>-results.txt in batch order at the end.
    python tools/test/sweep_par.py --items-file build/par-items.txt [--emus a,b,c,d] [--data out/world]
  items (one a line): <prefix>-<n> (a sweep batch, checked by questrun.py) or test:<name> (tools/test/cases)
Progress: build/par/progress.txt
"""
import argparse
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
import run_test

ROOT = mwtools.ROOT


def run_item(it, emu, app, data, start, log, qlogs, par):
    if it.startswith("test:"):
        t = it[5:]
        out = run_test.run(t, start=start, data=data, wait=1500, app=app, emu=emu,
                           pattern=r"expect: FAIL|drive: FAIL|monitor:|refused|not supported")
        (par / f"{t}.result").write_text("\n".join(out) + "\n", encoding="utf-8")
        if log.exists():
            shutil.copy2(log, par / f"{t}.log")
        return
    run_test.run(it, start=start, data=data, wait=2700, app=app, emu=emu, pattern="zzzz")
    if log.exists():
        shutil.copy2(log, qlogs / f"{it}.log")
    emu_log = Path(emu) / "user" / "log" / "azahar_log.txt"
    if emu_log.exists():
        shutil.copy2(emu_log, qlogs / f"{it}.emu.log")
    r = subprocess.run([sys.executable, str(mwtools.TEST_DIR / "questrun.py"), "--check", it,
                        "--log", str(qlogs / f"{it}.log")], capture_output=True, text=True, errors="replace")
    (par / f"{it}.result").write_text(r.stdout, encoding="utf-8")


def worker(emu, app, data, start, items, lock, par):
    log = Path(emu) / "user" / "sdmc" / "3ds" / "mw3ds" / "log.txt"
    qlogs = ROOT / "build" / "qrun-logs"
    while True:
        with lock:
            if not items:
                return
            it = items.pop(0)
        # One item failing (an emulator that died at boot and wrote no log, ...) must not stop this emulator's worker:
        # the error goes in the item's result and the worker takes the next one
        try:
            run_item(it, emu, app, data, start, log, qlogs, par)
        except Exception as e:
            name = it[5:] if it.startswith("test:") else it
            (par / f"{name}.result").write_text(f"{it}: error: {type(e).__name__}: {e}\n", encoding="utf-8")
        with lock:
            with open(par / "progress.txt", "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%H:%M')} {Path(emu).name}: {it} done\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--items-file", required=True)
    ap.add_argument("--emus", default="")
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--start", default="Seyda Neen")
    a = ap.parse_args()

    emus = a.emus or mwtools.get_env("MW3DS_EMU_DIRS", "comma separated Azahar folders")
    par = ROOT / "build" / "par"
    par.mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "qrun-logs").mkdir(parents=True, exist_ok=True)
    for f in [*par.glob("*.claim"), par / "progress.txt"]:
        f.unlink(missing_ok=True)
    items = [ln.strip() for ln in mwtools.read_lines(a.items_file) if ln.strip()]
    mwtools.write_lines(par / "queue.txt", items)
    # the binary as it is now (rebuilding meanwhile doesn't change what the batches run)
    app = ROOT / "build" / "par-app.3dsx"
    shutil.copy2(ROOT / "mw3ds.3dsx", app)
    queue, lock, threads = list(items), threading.Lock(), []
    for e in emus.split(","):
        t = threading.Thread(target=worker, args=(e.strip(), str(app), a.data, a.start, queue, lock, par))
        t.start()
        threads.append(t)
        time.sleep(20)          # staggered: four emulators booting at once load the disk together
    for t in threads:
        t.join()
    prefixes = sorted({re.sub(r"-\d+$", "", it) for it in items if not it.startswith("test:")})
    for prefix in prefixes:
        files = sorted(par.glob(f"{prefix}-*.result"), key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
        with open(ROOT / "build" / f"{prefix}-results.txt", "a", encoding="utf-8") as out:
            for f in files:
                out.write(f.read_text(encoding="utf-8"))
    print("sweep done")


if __name__ == "__main__":
    main()
