"""The converter image's start-up (tools/release/Dockerfile): turns the Morrowind install mounted at /morrowind into a
CIA in /out, as the Windows converter's window does, for Linux, the Steam Deck and anything else with podman or Docker.
    podman run --rm -v <Morrowind folder>:/morrowind:ro -v <output folder>:/out ghcr.io/3ds-conspiracy/mw3ds-converter
        [--old3ds] [--no-mods] [--tribunal] [--bloodmoon] [--encoding win1250] [--plugins "a.esp;b.esp"]
The image holds the converter (app/: this repository's layout, with the engine the release built), tex3ds and makerom.
The work is done in <output folder>/work: app/ is copied there and the conversion writes its data and caches inside it
(several GB), so a later run with the same output folder is much faster. The CIA is copied to <output folder>.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

APP = Path(os.environ.get("MW3DS_APP", "/opt/mw3ds/app"))
MORROWIND = Path(os.environ.get("MW3DS_MORROWIND_DIR", "/morrowind"))
OUT = Path(os.environ.get("MW3DS_OUT_DIR", "/out"))
FFMPEG_LOCK = APP / "ffmpeg.lock.json"                 # (tools/release/build_image.py: the Linux wheel's pin)
ENCODINGS = ("win1252", "win1250", "win1251")          # (wizard.py LANGUAGES: English and most others, Central European, Cyrillic)


def download(url, dest):
    import urllib.request
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "mw3ds-converter"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)


def fetch_ffmpeg(work):
    """ffmpeg for the game's movies. The image does not ship it (a GPL build: tools/release/make_release.py
    ffmpeg_lock); it is downloaded once from PyPI, in the imageio-ffmpeg wheel the image pins, checked against the
    pinned SHA-256, and kept in the work folder."""
    lock = json.loads(FFMPEG_LOCK.read_text(encoding="utf-8"))
    exe = work / "ffmpeg" / f"ffmpeg-{lock['sha256'][:12]}"
    if exe.is_file():
        return exe
    print(f"downloading ffmpeg for the movies from PyPI ({lock['size'] / 1048576:.0f} MB, the first time only)",
          flush=True)
    wheel = work / "ffmpeg" / "imageio-ffmpeg.whl"
    try:
        download(lock["url"], wheel)
    except OSError as e:
        raise SystemExit(f"could not download ffmpeg ({e}): check the network")
    if hashlib.sha256(wheel.read_bytes()).hexdigest() != lock["sha256"]:
        wheel.unlink()
        raise SystemExit("the ffmpeg download does not match its checksum: try again later")
    with zipfile.ZipFile(wheel) as z:
        name = next(n for n in z.namelist() if re.fullmatch(r"imageio_ffmpeg/binaries/ffmpeg-[^/]*", n))
        part = exe.with_suffix(".part")
        part.write_bytes(z.read(name))
        part.chmod(0o755)
        part.replace(exe)
    wheel.unlink()
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old3ds", action="store_true", help="for an Old 3DS / 2DS (smaller textures)")
    ap.add_argument("--no-mods", action="store_true", help="without the optimization mods (they make it run faster on "
                                                           "the 3DS; downloaded from their own pages the first time)")
    ap.add_argument("--tribunal", action="store_true", help="with Tribunal (Tribunal.esm and .bsa in Data Files)")
    ap.add_argument("--bloodmoon", action="store_true", help="with Bloodmoon (Bloodmoon.esm and .bsa in Data Files)")
    ap.add_argument("--encoding", default="win1252", choices=ENCODINGS,
                    help="your game's code page: win1252 for English, German, French, Italian and Spanish, win1250 "
                         "for Polish, Czech and Hungarian, win1251 for Russian and Ukrainian")
    ap.add_argument("--plugins", default="", help="plugins (.esp) to fold in, in load order, ';' between them")
    ap.add_argument("--small", action="store_true", help=argparse.SUPPRESS)     # (a trial of the image: Balmora only)
    a = ap.parse_args()

    version = (APP / "VERSION.txt").read_text().strip() if (APP / "VERSION.txt").is_file() else "dev"
    print(f"MW3DS Converter {version}", flush=True)
    if not (MORROWIND / "Data Files").is_dir():
        raise SystemExit(f"no Data Files folder in {MORROWIND}: mount your Morrowind folder there "
                         f"(-v <the folder holding Morrowind.exe>:/morrowind:ro)")
    if not OUT.is_dir():
        raise SystemExit(f"no {OUT}: mount a folder for the CIA there (-v <a folder>:/out)")
    for addon, wanted in (("Tribunal", a.tribunal), ("Bloodmoon", a.bloodmoon)):
        if wanted and not any(p.name.lower() == f"{addon.lower()}.esm" for p in (MORROWIND / "Data Files").iterdir()):
            raise SystemExit(f"--{addon.lower()}: there is no {addon}.esm in your Data Files")

    # The repo layout the build scripts expect, refreshed from the image at every run (the caches stay)
    work = OUT / "work"
    for p in APP.rglob("*"):
        if p.is_file() and "__pycache__" not in p.parts:
            d = work / p.relative_to(APP)
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, d)
    (work / "build").mkdir(exist_ok=True)
    env_file = work / ".env"
    if not env_file.exists():
        env_file.write_text("# The converter image passes its settings directly; this file only stops a warning.\n")

    env = dict(os.environ)
    env.update(MW3DS_MORROWIND_DIR=str(MORROWIND), MW3DS_ENCODING=a.encoding, MW3DS_PLUGINS=a.plugins,
               IMAGEIO_FFMPEG_EXE=str(fetch_ffmpeg(work)))
    for k in ("MW3DS_OUT", "MW3DS_OVERLAY_DIRS"):
        env.pop(k, None)
    cmd = [sys.executable, "tools/build/build_cia.py", "--prebuilt"]
    cmd += [] if a.no_mods else ["--mods"]
    cmd += ["--old3ds"] if a.old3ds else []
    cmd += ["--tribunal"] if a.tribunal else []
    cmd += ["--bloodmoon"] if a.bloodmoon else []
    cmd += ["--small"] if a.small else []
    print("running: " + " ".join(cmd[1:]), flush=True)
    r = subprocess.run(cmd, cwd=work, env=env)
    if r.returncode != 0:
        return r.returncode

    sys.path.insert(0, str(work / "tools" / "build"))
    from make_cia import names
    cia = work / names(a.old3ds, a.tribunal, a.bloodmoon)[1]
    shutil.copy2(cia, OUT / cia.name)
    print(f"done: {cia.name} is in your output folder. Install it with FBI.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
