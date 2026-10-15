"""Makes a bundled starting-point save: starts the game in a cell (character creation skipped), lets it autosave, and
copies the save into out/data/saves/ so it ships in the RomFS and shows on the title / Saves screens. Re-run after
re-converting the level (saves point at refs by index).
    python tools/test/make_save.py --cell Balmora --name balmora --title "Balmora"
    python tools/test/make_save.py --cell "Vivec, Foreign Quarter" --name vivec --title Vivec --data out/world --gold 5000
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
import run_emu

ROOT = mwtools.ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cell", default="Balmora")
    ap.add_argument("--name", default="balmora")
    ap.add_argument("--title", default="Balmora")
    ap.add_argument("--wait", type=int, default=150)
    ap.add_argument("--data", default="out/data")
    ap.add_argument("--gold", type=int, default=0)
    a = ap.parse_args()

    emu = Path(mwtools.get_env("MW3DS_EMU_DIR", "the Azahar folder"))
    sd = emu / "user" / "sdmc" / "3ds" / "mw3ds"
    out_dir = ROOT / a.data / "saves"
    for f in (sd / "save.json", sd / "save.json.meta"):
        f.unlink(missing_ok=True)
    inputs = ["3 0 0 0"]
    if a.gold > 0:
        inputs += [f"0.3 0 0 0 0 GIVE:gold_001:{a.gold}", "1 0 0 0"]
    run_emu.run(inputs=inputs, wait=a.wait, start=a.cell, data=a.data)

    save = sd / "save.json"
    if not save.exists():
        print("no save written - see the log")
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{a.name}.json"
    target.write_bytes(save.read_bytes())
    # name \t place \t level \t time: time 0 = no date shown
    (out_dir / f"{a.name}.json.meta").write_text(f"{a.title}\t{a.cell}\t1\t0", encoding="ascii", newline="")
    print(f"bundled save: {a.data}/saves/{a.name}.json ({round(target.stat().st_size / 1024)} KB)")
    for ln in mwtools.grep(sd / "log.txt", r"save:|load "):
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
