"""Builds build/mw3ds.cia from scratch: the program, the game data converted from your Morrowind install, and the CIA.
    python tools/build/build_cia.py                 whole island (about 30 min the first time, minutes after that)
    python tools/build/build_cia.py --small         only the area around Balmora (quick, for a first try)
    python tools/build/build_cia.py --skip-convert  reuse the data already in out/data
    python tools/build/build_cia.py --old3ds        for an Old 3DS: textures at most 128 texels (a quarter of the memory),
                                                    data in out/o3ds, build/mw3ds-o3ds.cia (the program is the same: it
                                                    finds the console itself)
    python tools/build/build_cia.py --mods          with the community optimization mods laid over the game files
                                                    (build_cia_mods.py is the same as --mods)
Needs .env filled in (see .env.example): MW3DS_MORROWIND_DIR, MW3DS_CTR_TOOLS_DIR, and on Windows MW3DS_MSYS2_DIR.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
from make_cia import build

ROOT = mwtools.ROOT

# The folders laid over Data Files, in load order (a later one wins when two ship the same file)
OVERLAYS = [
    "mods/morrowind-optimization-patch/00 Core",
    "mods/project-atlas/00 Core",
    "mods/project-atlas/01 Textures - Vanilla",
]


def fetch_mods():
    """Checks out the mods (git submodules, pinned to a commit each) and returns the overlay folders."""
    print("== the mods (git submodules)")
    r = subprocess.run(["git", "submodule", "update", "--init", "--depth", "1",
                        "mods/morrowind-optimization-patch", "mods/project-atlas"], cwd=ROOT)
    if r.returncode != 0:
        raise SystemExit("git submodule update failed (network? see the message above)")
    dirs = []
    for o in OVERLAYS:
        p = ROOT / o
        if not p.exists():
            raise SystemExit(f"overlay folder '{o}' is missing: is the submodule checked out?")
        print(f"   overlay: {o}")
        dirs.append(str(p))
    return dirs


def run(small=False, skip_convert=False, old3ds=False, mods=False, fetch_only=False):
    os.chdir(ROOT)
    mwtools.load_env()
    if mods:
        dirs = fetch_mods()
        if fetch_only:
            return 0
        # The converter (tools/convert/mwfiles.py) lays these folders over Data Files; a real environment variable wins over .env
        os.environ["MW3DS_OVERLAY_DIRS"] = ";".join(dirs)

    # Check the setup before the long steps
    morrowind = Path(mwtools.get_env("MW3DS_MORROWIND_DIR", "the folder of your Morrowind install"))
    ctr = Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", "the folder with makerom"))
    if mwtools.IS_WINDOWS:
        mwtools.get_env("MW3DS_MSYS2_DIR", "your MSYS2 folder")
    if not (morrowind / "Data Files" / "Morrowind.esm").exists():
        raise SystemExit(f"Morrowind.esm not found under {morrowind}/Data Files")
    if not (ctr / ("makerom.exe" if mwtools.IS_WINDOWS else "makerom")).exists():
        raise SystemExit(f"makerom not found in {ctr}")

    data = "out/o3ds" if old3ds else "out/data"
    cia = "build/mw3ds-o3ds.cia" if old3ds else "build/mw3ds.cia"
    convert_args = [] if small else ["--world"]
    if old3ds:
        convert_args += ["--max-tex", "128"]
        os.environ["MW3DS_OUT"] = "out/o3ds"

    # A CIA left by an earlier failed build must not be mistaken for this one
    (ROOT / cia).unlink(missing_ok=True)

    # 1. Convert the game data (make_cia builds the program itself)
    if not skip_convert:
        print(f"== converting game data (python {sys.version.split()[0]})")
        r = subprocess.run([sys.executable, "tools/convert/level.py", *convert_args], cwd=ROOT)
        if r.returncode != 0:
            raise SystemExit(f"level.py failed (exit code {r.returncode}). Send the whole output: the last 'decoding' line "
                             "names the sound file it stopped on.")
    elif not (ROOT / data).exists():
        raise SystemExit(f"{data} does not exist: run without --skip-convert first")

    # 2. Program + CIA
    print("== building the program and the CIA")
    if build(data, cia) != 0:
        raise SystemExit("CIA build failed: see the make / makerom output just above this error")
    print(f"Built {cia}.")
    return 0


def main(mods=False):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--small", action="store_true")
    ap.add_argument("--skip-convert", action="store_true")
    ap.add_argument("--old3ds", action="store_true")
    if not mods:
        ap.add_argument("--mods", action="store_true")
    ap.add_argument("--fetch-only", action="store_true", help="with --mods: just download the mods and print the overlay folders")
    a = ap.parse_args()
    return run(a.small, a.skip_convert, a.old3ds, mods or getattr(a, "mods", False), a.fetch_only)


if __name__ == "__main__":
    sys.exit(main())
