"""Assembles the release converter: a folder a player unzips and runs (MW3DS Converter.exe) to turn their own copy of
Morrowind into a CIA, with no Python, MSYS2, devkitPro or git of their own. Nothing from Morrowind is in it.
    python tools/release/make_release.py              build/release/MW3DS-Converter
    python tools/release/make_release.py --zip        and build/release/MW3DS-Converter-<version>.zip
    python tools/release/make_release.py --elf <dir>  take mw3ds.elf / mw3ds.smdh from <dir> (default: the repo root)
    python tools/release/make_release.py --build-tools-zip
                                                      only build/release/build-tools-windows.zip (tex3ds.exe, makerom.exe)
                                                      and its SHA-256, for the "build-tools" release release.yml uses
Run it with the Python version the release should carry (3.13: see requirements.txt): the embeddable Python of the
same version is downloaded from python.org, the packages are installed from requirements.txt, and tkinter is
copied from this Python. tex3ds and makerom come from where the build finds them (TEX3DS / devkitPro,
MW3DS_CTR_TOOLS_DIR). Windows only: the converter is a Windows program.

The folder it writes:
    MW3DS Converter.exe     starts python\\pythonw.exe on app\\tools\\release\\wizard.py (launcher.cs)
    python\\                 embeddable Python + numpy, pillow, miniaudio, imageio-ffmpeg ... + tkinter (ffmpeg itself
                            is not shipped: the wizard downloads it, see ffmpeg_lock)
    app\\                    the repo's own files the conversion needs, in the repo's layout (the wizard copies
                            them into the player's work folder and runs tools/build/build_cia.py --prebuilt there)
    bin\\                    tex3ds.exe, makerom.exe
    licenses\\               ours and the bundled programs'

A release is built in CI (.github/workflows/release.yml), never on a personal machine: a locally built mw3ds.elf
carries the build folder's path in its debug information. The last step fails the build when the user's home
folder (or a string in MW3DS_PRIVATE_STRINGS, ";"-separated) appears in any file.
"""
import argparse
import io
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
NAME = "MW3DS-Converter"
CSC = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Microsoft.NET" / "Framework64" / "v4.0.30319" / "csc.exe"

# The repo's files the conversion runs on (app\ keeps the repo's layout: mwtools.ROOT and envfile.ROOT are app\)
APP_FILES = [
    "tools/mwtools.py",
    "tools/build/build_cia.py", "tools/build/make_cia.py", "tools/build/mw3ds.rsf", "tools/build/banner.bnr",
    "tools/release/wizard.py",
    # (mwscript.engine_functions reads the engine's script function table at convert time)
    "source/components/interpreter/scriptfunctions.hpp",
    "LICENSE", "LANGUAGES.md", "requirements.txt",
]

THIRD_PARTY = """Programs bundled with the MW3DS Converter, and where their source is:

Python {python} (Python Software Foundation License): https://www.python.org/ (python/LICENSE.txt)
  with the packages in python/Lib/site-packages, each with its license in its *.dist-info folder.
  imageio-ffmpeg is there without its ffmpeg build: the converter downloads that from PyPI (the package's
  win_amd64 wheel, pinned in app/ffmpeg.lock.json) on your PC the first time, for the game's movies.
Tcl/Tk (BSD-style license), for the window: https://www.tcl.tk/
tex3ds (GPLv3), devkitPro: https://github.com/devkitPro/tex3ds
makerom (MIT), Project_CTR: https://github.com/3DSGuy/Project_CTR
  their license texts, the libraries tex3ds is built with, and where each one's source is: tex3ds-makerom/

The mods the converter can download when you build (Morrowind Optimization Patch, and Project Atlas for the Old 3DS
build) are not part of this download: they come from their own repositories, at the commits listed in
app/mods.lock.json.
"""

# Where the source of tex3ds.exe and makerom.exe is (GPLv3 section 6(d): clear directions next to the object code).
# The license texts are in tools/release/licenses, fetched from each project at these tags.
TOOL_SOURCES = """tex3ds.exe and makerom.exe: where their source is

tex3ds v2.3.0 (GNU GPL version 3, tex3ds-COPYING.txt), the Windows build devkitPro distributes.
  Source: https://github.com/devkitPro/tex3ds/tree/v2.3.0
  (archive: https://github.com/devkitPro/tex3ds/archive/refs/tags/v2.3.0.tar.gz)
  It is statically linked with these libraries, each under its own license (files in this folder):
    ImageMagick 7.0.7-31 (ImageMagick-LICENSE.txt): https://github.com/ImageMagick/ImageMagick/tree/7.0.7-31
    libpng 1.6.37 (libpng-LICENSE.txt): https://github.com/glennrp/libpng/tree/v1.6.37
    libjpeg-turbo 1.5.3 (libjpeg-turbo-LICENSE.md): https://github.com/libjpeg-turbo/libjpeg-turbo/tree/1.5.3
    zlib 1.2.11 (zlib-LICENSE.txt): https://zlib.net/fossils/
    FreeType, under the FreeType License (freetype-FTL.txt): https://freetype.org/
    bzip2 (bzip2-LICENSE.txt): https://sourceware.org/bzip2/
    mingw-w64 winpthreads v8.0.0 (winpthreads-COPYING.txt): https://www.mingw-w64.org/

makerom v0.19.0 (MIT License, makerom-LICENSE.txt), from Project_CTR.
  Source: https://github.com/3DSGuy/Project_CTR/tree/makerom-v0.19.0
"""


def copy_tool_licenses(dest):
    """The license texts of tex3ds, makerom and what tex3ds links, and where their source is, into `dest`."""
    dest.mkdir(parents=True, exist_ok=True)
    for p in sorted((Path(__file__).parent / "licenses").iterdir()):
        shutil.copy2(p, dest / p.name)
    (dest / "SOURCES-tex3ds-makerom.txt").write_text(TOOL_SOURCES, encoding="utf-8")


README = """MW3DS Converter {version}

Turns your own copy of The Elder Scrolls III: Morrowind (Steam, GOG or disc) into a CIA for a New 3DS or
Old 3DS with custom firmware. Nothing from Morrowind is in this download: the game comes from your install.

1. Unzip this whole folder somewhere with about 5 GB free.
2. Run "MW3DS Converter.exe". Windows may warn about an unknown publisher: More info, Run anyway.
3. Pick your Morrowind folder, your console and your language, then Start. The first run takes an hour or more;
   later runs reuse what was converted.
4. Install the CIA with FBI (or copy the data folder to the SD card, if you chose that).

The CIA holds your own copy of the game: it is for your console only.
MW3DS is a fan project, not affiliated with or endorsed by Bethesda Softworks.
Source code, help and bug reports: https://github.com/3ds-conspiracy/mw3ds
"""


def version():
    r = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip() or "dev"


def download(url, cache):
    """A file from `url`, kept in `cache` so a second run needs no network."""
    path = cache / url.rsplit("/", 1)[1]
    if not path.is_file():
        print(f"downloading {url}", flush=True)
        req = urllib.request.Request(url, headers={"User-Agent": "mw3ds-release"})
        data = urllib.request.urlopen(req, timeout=120).read()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return path


def make_python(dest, cache):
    """The embeddable Python of this interpreter's version, with the packages and tkinter."""
    ver = platform.python_version()
    if platform.machine().lower() not in ("amd64", "x86_64"):
        raise SystemExit("run this with 64-bit Python on x64 Windows")
    zipfile.ZipFile(download(f"https://www.python.org/ftp/python/{ver}/python-{ver}-embed-amd64.zip", cache)).extractall(dest)
    tag = f"python{sys.version_info.major}{sys.version_info.minor}"
    # Isolated as the embeddable Python always is (no PYTHONPATH, no registry, no user site), plus Lib for tkinter,
    # site-packages, and site (which runs sitecustomize.py: the script's folder on sys.path, line-buffered output)
    (dest / f"{tag}._pth").write_text(f"{tag}.zip\n.\nLib\nLib\\site-packages\nimport site\n", encoding="ascii")
    shutil.copy2(Path(__file__).parent / "sitecustomize.py", dest / "sitecustomize.py")

    # --no-compile: pip would write .pyc files carrying this machine's install path
    print("installing the packages", flush=True)
    site = dest / "Lib" / "site-packages"
    r = subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--no-compile", "--disable-pip-version-check",
                        "--no-warn-conflicts", "--only-binary=:all:", "--target", str(site),
                        "-r", str(ROOT / "requirements.txt")])
    if r.returncode != 0:
        raise SystemExit("pip install failed")
    # (the packages' command-line launchers: unused, and each holds the path of the Python that installed it)
    shutil.rmtree(site / "bin", ignore_errors=True)
    ffmpeg = ffmpeg_lock(site)

    base = Path(sys.base_prefix)
    for f in ("_tkinter.pyd", "tcl86t.dll", "tk86t.dll", "zlib1.dll"):
        src = base / "DLLs" / f
        if src.is_file():
            shutil.copy2(src, dest / f)
        elif f != "zlib1.dll":
            raise SystemExit(f"{src} not found: this Python has no tkinter (install it with the Tcl/Tk option)")
    shutil.copytree(base / "Lib" / "tkinter", dest / "Lib" / "tkinter",
                    ignore=shutil.ignore_patterns("__pycache__", "test"))
    shutil.copytree(base / "tcl", dest / "tcl", ignore=shutil.ignore_patterns("tix*", "*demos*"))
    return ffmpeg


def ffmpeg_lock(site):
    """Takes ffmpeg out of the bundled imageio-ffmpeg and pins the PyPI wheel it comes in, for the wizard to download
    on the player's PC. The binary is a GPL build of ffmpeg statically linked with many libraries (x264, x265, ...):
    shipping it would mean shipping all their source, so it is never part of the release."""
    dist = next(site.glob("imageio_ffmpeg-*.dist-info"))
    ver = dist.name[len("imageio_ffmpeg-"):-len(".dist-info")]
    for exe in (site / "imageio_ffmpeg" / "binaries").glob("ffmpeg*"):
        exe.unlink()
    req = urllib.request.Request(f"https://pypi.org/pypi/imageio-ffmpeg/{ver}/json", headers={"User-Agent": "mw3ds-release"})
    files = json.load(urllib.request.urlopen(req, timeout=60))["urls"]
    wheel = next(f for f in files if f["filename"].endswith("-win_amd64.whl"))
    return {"url": wheel["url"], "sha256": wheel["digests"]["sha256"], "size": wheel["size"]}


def make_app(dest, elf_dir):
    for rel in APP_FILES:
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, dest / rel)
    convert = dest / "tools" / "convert"
    convert.mkdir(parents=True, exist_ok=True)
    for p in sorted((ROOT / "tools" / "convert").glob("*.py")):
        shutil.copy2(p, convert / p.name)
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        src = Path(elf_dir) / f
        if not src.is_file():
            raise SystemExit(f"{src} not found: build the program first (tools/build/make.py), or pass --elf")
        shutil.copy2(src, dest / f)

    # The mods at the commits the submodules pin (build_cia.py downloads them as zips from these)
    urls = dict(re.findall(r"path = (\S+)\s+url = (\S+)", (ROOT / ".gitmodules").read_text(encoding="utf-8")))
    tree = subprocess.run(["git", "ls-tree", "HEAD", "mods/"], cwd=ROOT, capture_output=True, text=True, check=True)
    lock = {}
    for line in tree.stdout.splitlines():
        mode, kind, sha, path = line.split(None, 3)
        if kind == "commit":
            lock[path] = {"url": urls[path], "commit": sha}
    (dest / "mods.lock.json").write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")


def make_bin(dest):
    sys.path.insert(0, str(ROOT / "tools" / "convert"))
    os.environ.setdefault("MW3DS_MORROWIND_DIR", ".")      # (textures imports mwfiles, which wants it set)
    mwtools.load_env()
    from textures import tex3ds_path
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(tex3ds_path(), dest / "tex3ds.exe")
    shutil.copy2(Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", "the folder with makerom.exe")) / "makerom.exe",
                 dest / "makerom.exe")


def make_launcher(dest):
    if not CSC.is_file():
        raise SystemExit(f"{CSC} not found: the launcher is built with the .NET Framework 4 C# compiler")
    from PIL import Image
    icon = dest.parent / "launcher.ico"
    Image.open(ROOT / "icon.png").convert("RGBA").save(icon, sizes=[(16, 16), (32, 32), (48, 48)])
    r = subprocess.run([str(CSC), "/nologo", "/target:winexe", "/optimize+", f"/win32icon:{icon}",
                        f"/out:{dest}", str(Path(__file__).parent / "launcher.cs")])
    icon.unlink()
    if r.returncode != 0:
        raise SystemExit("the launcher did not compile")


def check_private(folder):
    """Fails when the home folder of whoever built this, or a private string, is in any file of the release."""
    home = Path.home()
    needles = {str(home), str(home).replace("\\", "/"), str(home).replace("\\", "\\\\")}
    # (on a GitHub runner the home folder is C:\Users\runneradmin, nobody's own: numpy's wheels are built on the same
    # runners and carry it in their build configuration)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        needles = set()
    needles |= {s for s in os.environ.get("MW3DS_PRIVATE_STRINGS", "").split(";") if s.strip()}
    pats = []
    for s in needles:
        pats.append(s.lower().encode("utf-8"))
        pats.append(s.lower().encode("utf-16-le"))
    hits = []
    for p in folder.rglob("*"):
        if p.is_file():
            data = p.read_bytes().lower()
            if any(n in data for n in pats):
                hits.append(p.relative_to(folder))
    if hits:
        print("private strings found in the release (it must not be published):")
        for h in hits:
            print(f"  {h}")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="build/release")
    ap.add_argument("--elf", default=str(ROOT), help="folder holding mw3ds.elf and mw3ds.smdh")
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--no-check", action="store_true", help="skip the private string check (local trial builds only)")
    ap.add_argument("--build-tools-zip", action="store_true")
    a = ap.parse_args()
    if not mwtools.IS_WINDOWS:
        raise SystemExit("the release converter is built on Windows")

    out = (ROOT / a.out).resolve()
    if a.build_tools_zip:
        import hashlib
        tools = out / "build-tools"
        shutil.rmtree(tools, ignore_errors=True)
        make_bin(tools)
        copy_tool_licenses(tools)
        archive = out / "build-tools-windows.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for p in sorted(tools.iterdir()):
                z.write(p, p.name)
        if not a.no_check and not check_private(tools):
            return 1
        print(f"{archive}\nSHA-256 {hashlib.sha256(archive.read_bytes()).hexdigest()}")
        return 0

    folder = out / NAME
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    ver = version()

    ffmpeg = make_python(folder / "python", out / "cache")
    make_app(folder / "app", a.elf)
    (folder / "app" / "VERSION.txt").write_text(ver + "\n", encoding="utf-8")
    (folder / "app" / "ffmpeg.lock.json").write_text(json.dumps(ffmpeg, indent=1) + "\n", encoding="utf-8")
    make_bin(folder / "bin")
    make_launcher(folder / "MW3DS Converter.exe")
    lic = folder / "licenses"
    lic.mkdir()
    shutil.copy2(ROOT / "LICENSE", lic / "MW3DS-LICENSE.txt")
    (lic / "THIRD-PARTY.txt").write_text(THIRD_PARTY.format(python=platform.python_version()), encoding="utf-8")
    copy_tool_licenses(lic / "tex3ds-makerom")
    (folder / "README.txt").write_text(README.format(version=ver), encoding="utf-8")

    if not a.no_check and not check_private(folder):
        return 1
    size = sum(p.stat().st_size for p in folder.rglob("*") if p.is_file())
    print(f"built {folder} ({size / 1048576:.0f} MB), version {ver}")
    if a.zip:
        archive = out / f"{NAME}-{ver}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(folder.rglob("*")):
                z.write(p, Path(NAME) / p.relative_to(folder))
        print(f"zipped {archive} ({archive.stat().st_size / 1048576:.0f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
