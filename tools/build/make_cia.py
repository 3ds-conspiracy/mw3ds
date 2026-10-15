"""Builds build/mw3ds.cia with the converted game data packed into its RomFS.
New 3DS settings: 124MB app memory mode, 804MHz CPU, L2 cache.
    python tools/build/make_cia.py [--no-data] [--data out/world] [--out build/mw3ds-o3ds.cia] [--prebuilt]
  --no-data   the program only (~2 MB): the game reads its data from sdmc:/3ds/mw3ds/data
              (the whole island copied there by card reader); no Wi-Fi update checks either
  --data      pack that data (default out/data, the Balmora area)
  --out       write the CIA there (default build/mw3ds.cia)
  --prebuilt  do not run make: pack the mw3ds.elf and mw3ds.smdh already in the repo root
The same program runs on both consoles (it takes its Old 3DS profile on an Old 3DS: 64 MB mode, see source/heaps.cpp);
only the data differs (build_cia.py --old3ds converts with quarter-size textures).
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
DEFS = {
    "APP_TITLE": "MW3DS", "APP_PRODUCT_CODE": "CTR-P-MW3D", "APP_UNIQUE_ID": "0xF8287",
    "APP_ROMFS": "build/romfs", "APP_CATEGORY": "Application", "APP_USE_ON_SD": "true", "APP_ENCRYPTED": "false",
    "APP_MEMORY_TYPE": "Application", "APP_SYSTEM_MODE": "64MB", "APP_SYSTEM_MODE_EXT": "124MB",
    "APP_CPU_SPEED": "804MHz", "APP_ENABLE_L2_CACHE": "true", "APP_VERSION_MAJOR": "0",
}


def build(data="out/data", out="build/mw3ds.cia", no_data=False):
    """Builds the program and the CIA; returns the exit code."""
    mwtools.load_env()
    made = mwtools.make([], capture=True)
    lines = made.stdout.splitlines()
    for ln in lines:
        if re.search("error|warning", ln, re.IGNORECASE):
            print(ln)
    if made.returncode != 0:
        print(f"make failed (exit code {made.returncode}). Last lines of its output:")
        print("\n".join(lines[-30:]))
        return 1
    return pack(data, out, no_data)


def pack(data="out/data", out="build/mw3ds.cia", no_data=False):
    """Packs the program already built (mw3ds.elf and mw3ds.smdh in the repo root) and the data into the CIA;
    returns the exit code. The release converter calls this with a prebuilt program (tools/release)."""
    mwtools.load_env()
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        if not (ROOT / f).is_file():
            print(f"{f} not found in {ROOT}: build the program first (tools/build/make.py)")
            return 1
    staged = ROOT / "build" / "romfs" / "data"
    shutil.rmtree(staged, ignore_errors=True)
    # (the RSF's RomFs root: empty for the program-only CIA, which makerom wants to exist all the same)
    staged.parent.mkdir(parents=True, exist_ok=True)
    if not no_data:
        mwtools.copy_tree_linked(ROOT / data, staged)
    makerom_name = "makerom.exe" if mwtools.IS_WINDOWS else "makerom"
    makerom = Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", f"the folder with {makerom_name}")) / makerom_name
    args = [str(makerom), "-f", "cia", "-o", str(out), "-elf", "mw3ds.elf", "-rsf", "tools/build/mw3ds.rsf",
            "-icon", "mw3ds.smdh", "-banner", "tools/build/banner.bnr", "-exefslogo", "-target", "t"]
    args += [f"-D{k}={v}" for k, v in DEFS.items()]
    code = subprocess.run(args, cwd=ROOT).returncode
    # The staged copy of the data is only makerom's input: packed into the CIA, it would be a third 1 GB copy on disk
    shutil.rmtree(staged, ignore_errors=True)
    if code != 0:
        print("makerom failed")
        return 1
    cia = ROOT / out
    print(f"built {cia} ({cia.stat().st_size / 1048576:.1f} MB)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-data", action="store_true")
    ap.add_argument("--data", default="out/data")
    ap.add_argument("--out", default="build/mw3ds.cia")
    ap.add_argument("--prebuilt", action="store_true")
    a = ap.parse_args()
    return (pack if a.prebuilt else build)(a.data, a.out, a.no_data)


if __name__ == "__main__":
    sys.exit(main())
