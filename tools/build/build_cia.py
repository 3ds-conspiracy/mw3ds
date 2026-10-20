"""Builds build/mw3ds.cia from scratch: the program, the game data converted from your Morrowind install, and the CIA.
    python tools/build/build_cia.py                 whole island (about 30 min the first time, minutes after that)
    python tools/build/build_cia.py --small         only the area around Balmora (quick, for a first try)
    python tools/build/build_cia.py --skip-convert  reuse the data already in out/data
    python tools/build/build_cia.py --old3ds        for an Old 3DS: textures at most 128 texels (a quarter of the memory),
                                                    data in out/o3ds, build/mw3ds-o3ds.cia (the program is the same: it
                                                    finds the console itself)
    python tools/build/build_cia.py --mods          with the community optimization mods laid over the game files
                                                    (build_cia_mods.py is the same as --mods)
    python tools/build/build_cia.py --no-data       the program only in the CIA (~2 MB); the data stays in out/... for
                                                    copying to sdmc:/3ds/mw3ds/data
    python tools/build/build_cia.py --prebuilt      pack the mw3ds.elf / mw3ds.smdh already in the repo root instead of
                                                    building them (the release converter, tools/release, uses this)
Needs .env filled in (see .env.example): MW3DS_MORROWIND_DIR, MW3DS_CTR_TOOLS_DIR, and on Windows MW3DS_MSYS2_DIR
(not with --prebuilt).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools
from make_cia import build, pack

ROOT = mwtools.ROOT

# The folders laid over Data Files, in load order (a later one wins when two ship the same file)
OVERLAYS = [
    "mods/morrowind-optimization-patch/00 Core",
    "mods/project-atlas/00 Core",
    "mods/project-atlas/01 Textures - Vanilla",
]
# Project Atlas draws many objects from one big sheet each: fewer draws. The New 3DS keeps its frame rate without it
# (Balmora: 30 fps either way, about 1.3 ms more GPU command time a frame) and draws everything at its own texture's
# detail; the Old 3DS, short of time for draws, keeps it, a sheet capped per picture in it (textures.py is_sheet: capped
# whole, Dunmer furniture came out about 16 x 16 texels)
ATLAS_OVERLAYS = ["mods/project-atlas/00 Core", "mods/project-atlas/01 Textures - Vanilla"]


# Outside a git checkout (the release converter's folder) the mods come as zips of the same commits the submodules
# pin, listed here by tools/release/make_release.py: {"mods/<name>": {"url": <repository>, "commit": <sha>}}
MODS_LOCK = "mods.lock.json"


def fetch_mod_zips():
    """Downloads each mod in mods.lock.json as a zip of its pinned commit (once: a folder already there is kept)."""
    print("== the mods (zips of the pinned commits)")
    for path, pin in json.loads((ROOT / MODS_LOCK).read_text(encoding="utf-8")).items():
        dest = ROOT / path
        stamp = dest / ".commit"
        if stamp.is_file() and stamp.read_text().strip() == pin["commit"]:
            continue
        url = pin["url"].removesuffix(".git") + f"/archive/{pin['commit']}.zip"
        print(f"   downloading {url}", flush=True)
        archive = dest.with_name(dest.name + ".zip")
        try:
            mwtools.download(url, archive)
        except OSError as e:
            raise SystemExit(f"could not download {url} ({e}): check the network (a firewall or antivirus may block "
                             f"Python), or build without the mods")
        shutil.rmtree(dest, ignore_errors=True)
        tmp = dest.with_name(dest.name + ".part")
        shutil.rmtree(tmp, ignore_errors=True)
        # GitHub's zip holds one folder, <repository>-<commit>: it is left out here rather than extracted and moved,
        # since its 40-letter commit would take the mod's deepest files past Windows' 260-letter path limit in a
        # work folder only a little deep (Downloads, OneDrive's Desktop)
        try:
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    rel = Path(*Path(info.filename).parts[1:])
                    if info.is_dir() or not rel.parts or ".." in rel.parts:
                        continue
                    out = tmp / rel
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as src, open(out, "wb") as f:
                        shutil.copyfileobj(src, f, 1 << 20)
        except (OSError, zipfile.BadZipFile) as e:
            shutil.rmtree(tmp, ignore_errors=True)
            raise SystemExit(f"could not unpack {archive.name} into {dest.parent} ({e}): a shorter work folder "
                             f"(such as C:\\MW3DS) helps if the path was too long")
        tmp.replace(dest)
        archive.unlink()
        stamp.write_text(pin["commit"] + "\n")


def fetch_mods(old3ds=False):
    """Checks out the mods (git submodules, pinned to a commit each) and returns the overlay folders (Project Atlas's
    for the Old 3DS only)."""
    if (ROOT / ".git").exists() or not (ROOT / MODS_LOCK).is_file():
        print("== the mods (git submodules)")
        r = subprocess.run(["git", "submodule", "update", "--init", "--depth", "1",
                            "mods/morrowind-optimization-patch", "mods/project-atlas"], cwd=ROOT)
        if r.returncode != 0:
            raise SystemExit("git submodule update failed (network? see the message above)")
    else:
        fetch_mod_zips()
    dirs = []
    for o in OVERLAYS:
        if o in ATLAS_OVERLAYS and not old3ds:
            continue
        p = ROOT / o
        if not p.exists():
            raise SystemExit(f"overlay folder '{o}' is missing: is the submodule checked out?")
        print(f"   overlay: {o}")
        dirs.append(str(p))
    return dirs


def run(small=False, skip_convert=False, old3ds=False, mods=False, fetch_only=False, no_data=False, prebuilt=False):
    os.chdir(ROOT)
    mwtools.load_env()
    if mods:
        dirs = fetch_mods(old3ds)
        if fetch_only:
            return 0
        # The converter (tools/convert/mwfiles.py) lays these folders over Data Files; a real environment variable wins over .env
        os.environ["MW3DS_OVERLAY_DIRS"] = ";".join(dirs)

    # Check the setup before the long steps
    morrowind = Path(mwtools.get_env("MW3DS_MORROWIND_DIR", "the folder of your Morrowind install"))
    ctr = Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", "the folder with makerom"))
    if mwtools.IS_WINDOWS and not prebuilt:
        mwtools.get_env("MW3DS_MSYS2_DIR", "your MSYS2 folder")
    if not (morrowind / "Data Files" / "Morrowind.esm").exists():
        raise SystemExit(f"Morrowind.esm not found under {morrowind}/Data Files")
    if not (ctr / ("makerom.exe" if mwtools.IS_WINDOWS else "makerom")).exists():
        raise SystemExit(f"makerom not found in {ctr}")

    data = "out/o3ds" if old3ds else "out/data"
    cia = "build/mw3ds-o3ds.cia" if old3ds else "build/mw3ds.cia"
    convert_args = [] if small else ["--world"]
    if old3ds:
        convert_args += ["--max-tex", "128", "--no-mips"]
        os.environ["MW3DS_OUT"] = "out/o3ds"

    # A CIA left by an earlier failed build must not be mistaken for this one
    (ROOT / cia).unlink(missing_ok=True)

    # 1. Convert the game data (make_cia builds the program itself)
    # (the same interpreter runs the converter: the release converter's own Python, or yours)
    if not skip_convert:
        print(f"== converting game data (python {sys.version.split()[0]})")
        r = subprocess.run([sys.executable, "tools/convert/level.py", *convert_args], cwd=ROOT)
        if r.returncode != 0:
            raise SystemExit(f"level.py failed (exit code {r.returncode}). Send the whole output: the last 'decoding' line "
                             "names the sound file it stopped on.")
    elif not (ROOT / data).exists():
        raise SystemExit(f"{data} does not exist: run without --skip-convert first")
    # The world map from the cells' top-down renders (the native build's; without one the drawn map stays)
    if not skip_convert:
        print("== rendering the world map from above")
        r = subprocess.run([sys.executable, "tools/convert/maprender.py", "--data", data], cwd=ROOT)
        if r.returncode != 0:
            print("(maprender failed: the map stays as level.py drew it)")

    # 2. Program + CIA
    print("== packing the CIA" if prebuilt else "== building the program and the CIA")
    if (pack if prebuilt else build)(data, cia, no_data) != 0:
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
    ap.add_argument("--no-data", action="store_true")
    ap.add_argument("--prebuilt", action="store_true")
    a = ap.parse_args()
    return run(a.small, a.skip_convert, a.old3ds, mods or getattr(a, "mods", False), a.fetch_only, a.no_data, a.prebuilt)


if __name__ == "__main__":
    sys.exit(main())
