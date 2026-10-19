"""Checks that a release carries its tag's version everywhere a player sees one: the converter's VERSION.txt, the CIA's
title version (FBI, System Settings > Data Management) that make_cia.py works out on the player's machine, and the
HOME menu's description in mw3ds.smdh. release.yml runs it before the draft release is made.
    python tools/release/check_version.py <tag> [<converter folder>]     (default build/release/MW3DS-Converter)
The tag is major.minor.micro (a leading "v" is allowed): the CIA's title version holds major and minor up to 63 and
micro up to 15, anything above is clamped and two releases would show the same number.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SMDH_TITLES, SMDH_TITLE_SIZE = 16, 0x200            # (each: short description, long description, publisher)


def smdh_descriptions(path):
    """The long description of every language in an SMDH file."""
    data = path.read_bytes()
    if data[:4] != b"SMDH":
        raise SystemExit(f"{path} is not an SMDH file")
    out = []
    for i in range(SMDH_TITLES):
        entry = data[8 + i * SMDH_TITLE_SIZE:8 + (i + 1) * SMDH_TITLE_SIZE]
        out.append(entry[0x80:0x180].decode("utf-16-le").split("\0", 1)[0])
    return out


def cia_version(app):
    """make_cia.app_version() as the converter runs it: from the app folder, with no MW3DS_VERSION and no git (a
    player's work folder has neither, so a missing VERSION.txt must not be covered by this repository's tag)."""
    env = {k: v for k, v in os.environ.items() if k != "MW3DS_VERSION"}
    env["GIT_DIR"] = str(app / "no-git")
    code = ("import sys; sys.path.insert(0, 'tools/build'); import make_cia; "
            "text, number = make_cia.app_version(); print(text, number)")
    r = subprocess.run([sys.executable, "-I", "-c", code], cwd=app, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines() or ["no output"])[-1]
    text, number = r.stdout.split()
    return text, int(number)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag")
    ap.add_argument("folder", nargs="?", default=str(ROOT / "build" / "release" / "MW3DS-Converter"))
    a = ap.parse_args()
    app = Path(a.folder) / "app"
    errors = []

    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", a.tag)
    if not m:
        raise SystemExit(f"tag {a.tag!r} is not major.minor.micro: the CIA's title version can't hold it")
    major, minor, micro = (int(g) for g in m.groups())
    plain = f"{major}.{minor}.{micro}"
    if major > 63 or minor > 63 or micro > 15:
        errors.append(f"tag {a.tag}: the CIA's title version holds major and minor up to 63 and micro up to 15")

    version_txt = app / "VERSION.txt"
    text = version_txt.read_text(encoding="utf-8").strip() if version_txt.is_file() else None
    if text != a.tag:
        errors.append(f"{version_txt}: {text!r}, not {a.tag!r} (a dirty or untagged checkout?)")

    text, number = cia_version(app)
    want = (major << 10) | (minor << 4) | micro
    if text is None:
        errors.append(f"make_cia.app_version() failed in {app}: {number}")
    elif text != plain or number != want:
        errors.append(f"CIA title version: {text} ({number}), not {plain} ({want})")

    smdh = app / "mw3ds.smdh"
    for desc in sorted(set(smdh_descriptions(smdh))):
        if not desc.endswith(" " + a.tag):
            errors.append(f"{smdh}: HOME menu description {desc!r} does not end with {a.tag!r}")

    for e in errors:
        print(f"error: {e}")
    if errors:
        return 1
    print(f"version {a.tag} everywhere: VERSION.txt, CIA title version {number}, HOME menu description")
    return 0


if __name__ == "__main__":
    sys.exit(main())
