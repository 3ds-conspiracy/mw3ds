"""Checks the build requirements and installs what it can:
    python tools/build/setup.py          install the Python packages and the devkitPro packages, then report what is left
    python tools/build/setup.py --check  only report, install nothing
Reads .env (created from .env.example if it is missing).
"""
import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
problems = 0


def ok(msg):
    print(f"  ok       {msg}")


def fix(msg):
    global problems
    print(f"  MISSING  {msg}")
    problems += 1


def check_python(check):
    print("== Python")
    if sys.version_info < (3, 10):
        fix("python 3.10+ (https://www.python.org/downloads/)")
        return
    ok(sys.version.split()[0])
    if check:
        r = subprocess.run([sys.executable, "-c", "import numpy, PIL, miniaudio, imageio_ffmpeg, psutil"], capture_output=True)
        if r.returncode == 0:
            ok("numpy, pillow, miniaudio, imageio-ffmpeg, psutil")
        else:
            fix("Python packages: run python tools/build/setup.py (pip install -r requirements.txt)")
    else:
        r = subprocess.run([sys.executable, "-m", "pip", "install", "-r", str(ROOT / "requirements.txt")])
        if r.returncode != 0:
            fix("pip install -r requirements.txt failed")
        else:
            ok("Python packages installed")


def check_morrowind():
    print("== Morrowind")
    mw = os.environ.get("MW3DS_MORROWIND_DIR")
    if not mw:
        fix("MW3DS_MORROWIND_DIR is not set in .env")
    elif (Path(mw) / "Data Files" / "Morrowind.esm").exists():
        ok(f"Morrowind.esm in {mw}")
    else:
        fix(f"Morrowind.esm not found under {mw}/Data Files")


def check_devkitpro(check):
    if mwtools.IS_WINDOWS:
        print("== MSYS2 + devkitPro")
        msys = os.environ.get("MW3DS_MSYS2_DIR")
        bash = Path(msys) / "usr" / "bin" / "bash.exe" if msys else None
        if not bash or not bash.exists():
            fix("MSYS2 (https://www.msys2.org/): install it and set MW3DS_MSYS2_DIR in .env")
            return
        if mwtools.devkitpro_bash("grep -q '^\\[dkp-libs\\]' /etc/pacman.conf").returncode != 0:
            fix("devkitPro is not set up in this MSYS2: follow https://devkitpro.org/wiki/devkitPro_pacman "
                "(add the repositories and keyring)")
        elif check:
            if mwtools.devkitpro_bash("pacman -Qg 3ds-dev >/dev/null 2>&1").returncode == 0:
                ok("3ds-dev packages")
            else:
                fix("3ds-dev packages: run python tools/build/setup.py")
        else:
            if mwtools.devkitpro_bash("pacman -S --needed --noconfirm 3ds-dev").returncode != 0:
                fix("pacman -S 3ds-dev failed")
            else:
                ok("3ds-dev packages installed")
        return
    print("== devkitPro")
    home = os.environ.get("DEVKITPRO") or "/opt/devkitpro"
    if not Path(home).is_dir():
        fix("devkitPro (https://devkitpro.org/wiki/Getting_Started): install it, then set DEVKITPRO")
    elif not (Path(home) / "libctru").is_dir():
        fix("3ds-dev packages: sudo dkp-pacman -S 3ds-dev")
    else:
        ok(f"devkitPro with libctru in {home}")


def makerom_name():
    return "makerom.exe" if mwtools.IS_WINDOWS else "makerom"


def download_makerom(dest):
    """The latest makerom release from Project_CTR into `dest`; returns the release tag."""
    req = urllib.request.Request("https://api.github.com/repos/3DSGuy/Project_CTR/releases", headers={"User-Agent": "mw3ds-setup"})
    releases = json.load(urllib.request.urlopen(req, timeout=60))
    rel = next(r for r in releases if r["tag_name"].startswith("makerom-"))
    if mwtools.IS_WINDOWS:
        wanted = ("win_x86_64",)
    elif sys.platform == "darwin":
        wanted = ("macos_arm64", "macos_x86_64", "macos")
    else:
        wanted = ("ubuntu_x86_64", "linux_x86_64", "linux")
    asset = next((a for w in wanted for a in rel["assets"] if w in a["name"]), None)
    if not asset:
        raise RuntimeError("no makerom build for this system in " + rel["tag_name"])
    data = urllib.request.urlopen(asset["browser_download_url"], timeout=120).read()
    dest.mkdir(parents=True, exist_ok=True)
    if asset["name"].endswith(".zip"):
        zipfile.ZipFile(io.BytesIO(data)).extractall(dest)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            try:
                t.extractall(dest, filter="data")      # (no member may land outside dest)
            except TypeError:                           # (Python before 3.10.12 has no filter)
                t.extractall(dest)
    exe = dest / makerom_name()
    if not exe.exists():
        raise RuntimeError(f"{makerom_name()} is not in the download")
    exe.chmod(exe.stat().st_mode | 0o111)
    return rel["tag_name"]


def set_env_line(key, value):
    """Fills in `key` in .env: its blank line from .env.example when there is one, else a new line at the end."""
    env = ROOT / ".env"
    lines = env.read_text(encoding="utf-8-sig").splitlines() if env.exists() else []
    for i, ln in enumerate(lines):
        if ln.strip() == f"{key}=":
            lines[i] = f"{key}={value}"
            break
    else:
        lines.append(f"{key}={value}")
    env.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check_makerom(check):
    print("== makerom (for the CIA)")
    ctr = os.environ.get("MW3DS_CTR_TOOLS_DIR")
    if ctr and (Path(ctr) / makerom_name()).exists():
        ok(f"{makerom_name()} in {ctr}")
    elif check:
        fix(f"{makerom_name()}: run python tools/build/setup.py to download it")
    else:
        dest = Path(ctr) if ctr else ROOT / ".tools" / "ctr"
        try:
            tag = download_makerom(dest)
            if not ctr:
                set_env_line("MW3DS_CTR_TOOLS_DIR", str(dest))
                os.environ["MW3DS_CTR_TOOLS_DIR"] = str(dest)
            ok(f"downloaded {tag} to {dest}")
        except Exception as e:
            fix(f"{makerom_name()} could not be downloaded ({e}): get it from https://github.com/3DSGuy/Project_CTR/releases")


def check_azahar():
    print("== Azahar (only for running tests / the emulator)")
    emu = os.environ.get("MW3DS_EMU_DIR")
    if emu and mwtools.emulator_exe(emu).exists():
        ok(f"{mwtools.emulator_exe(emu).name} in {emu}")
    else:
        print("  optional azahar: https://azahar-emu.org/ , portable install, then set MW3DS_EMU_DIR in .env")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only report, install nothing")
    a = ap.parse_args()
    # Our lines must come out in order with pip's and pacman's, which write straight to the console
    sys.stdout.reconfigure(line_buffering=True)
    os.chdir(ROOT)
    if not (ROOT / ".env").exists():
        shutil.copy(ROOT / ".env.example", ROOT / ".env")
        print("Created .env from .env.example: fill in the paths, then run this again.")
    mwtools.load_env()
    check_python(a.check)
    check_morrowind()
    check_devkitpro(a.check)
    check_makerom(a.check)
    check_azahar()
    print()
    if problems:
        print(f"{problems} thing(s) to fix above.")
        return 1
    print("Everything needed to build is in place.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
