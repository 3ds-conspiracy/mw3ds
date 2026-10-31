"""Builds the converter image (tools/release/Dockerfile): the release converter for Linux and the Steam Deck, run with
podman or Docker. The release workflow builds it from the engine CI built and pushes it to the GitHub registry.
    python tools/release/build_image.py --elf <dir>            stage build/image and build mw3ds-converter:dev
    python tools/release/build_image.py --elf <dir> --stage    only stage build/image (CI builds and pushes it)
    python tools/release/build_image.py --elf <dir> --tag ghcr.io/3ds-conspiracy/mw3ds-converter:0.2.0
<dir> holds mw3ds.elf and mw3ds.smdh: a stripped ELF, as CI's (a locally built one carries the build folder's path in
its debug information, and the private string check refuses it). podman builds it when installed, else docker.

The staged folder (the build context) holds only what the image needs:
    Dockerfile
    app/        the converter in the repo's layout, as the Windows release's app/ (make_release.make_app), with
                tools/release/container.py, VERSION.txt and ffmpeg.lock.json (the Linux ffmpeg wheel's pin)
    licenses/   tex3ds's and makerom's license texts and where their source is
"""
import argparse
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release                                    # (make_app, copy_tool_licenses, check_private, version)

ROOT = make_release.ROOT

# Where the source of the image's tex3ds and makerom is (GPLv3 section 6(d), as make_release.TOOL_SOURCES for Windows)
TOOL_SOURCES = """tex3ds and makerom in this image: where their source is

tex3ds v2.3.0 (GNU GPL version 3, tex3ds-COPYING.txt), the Linux build devkitPro distributes in its devkitpro/devkitarm
image. Source: https://github.com/devkitPro/tex3ds/tree/v2.3.0
  (archive: https://github.com/devkitPro/tex3ds/archive/refs/tags/v2.3.0.tar.gz)
  It is statically linked with the libraries it is built with, each under its own license (files in this folder):
  ImageMagick, libpng, libjpeg-turbo, zlib, FreeType and bzip2.

makerom v0.19.0 (MIT License, makerom-LICENSE.txt), Project_CTR's Linux build.
  Source: https://github.com/3DSGuy/Project_CTR/tree/makerom-v0.19.0

ffmpeg is not in this image: the converter downloads it from PyPI (the imageio-ffmpeg wheel pinned in
app/ffmpeg.lock.json) the first time it runs, for the game's movies.
"""


def ffmpeg_lock():
    """The pin of the newest imageio-ffmpeg wheel for 64-bit Linux, whose ffmpeg container.py downloads."""
    req = urllib.request.Request("https://pypi.org/pypi/imageio-ffmpeg/json", headers={"User-Agent": "mw3ds-release"})
    files = json.load(urllib.request.urlopen(req, timeout=60))["urls"]
    wheel = next(f for f in files if "manylinux" in f["filename"] and f["filename"].endswith("x86_64.whl"))
    return {"url": wheel["url"], "sha256": wheel["digests"]["sha256"], "size": wheel["size"]}


def stage(dest, elf, ver):
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    shutil.copy2(Path(__file__).parent / "Dockerfile", dest / "Dockerfile")
    app = dest / "app"
    make_release.make_app(app, elf)
    shutil.copy2(Path(__file__).parent / "container.py", app / "tools" / "release" / "container.py")
    (app / "VERSION.txt").write_text(ver + "\n", encoding="utf-8")
    (app / "ffmpeg.lock.json").write_text(json.dumps(ffmpeg_lock(), indent=1) + "\n", encoding="utf-8")
    make_release.copy_tool_licenses(dest / "licenses")
    (dest / "licenses" / "SOURCES-tex3ds-makerom.txt").write_text(TOOL_SOURCES, encoding="utf-8")
    shutil.copy2(ROOT / "LICENSE", dest / "licenses" / "MW3DS-LICENSE.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True, help="folder holding a stripped mw3ds.elf and mw3ds.smdh")
    ap.add_argument("--out", default="build/image")
    ap.add_argument("--tag", default="mw3ds-converter:dev")
    ap.add_argument("--version", default="", help="the version the converter shows (default: git describe)")
    ap.add_argument("--stage", action="store_true", help="only stage the build context")
    ap.add_argument("--no-check", action="store_true", help="skip the private string check (local trial builds only)")
    a = ap.parse_args()

    dest = (ROOT / a.out).resolve()
    stage(dest, a.elf, a.version or make_release.version())
    if not a.no_check and not make_release.check_private(dest):
        return 1
    print(f"staged {dest}")
    if a.stage:
        return 0
    engine = shutil.which("podman") or shutil.which("docker")
    if not engine:
        raise SystemExit("neither podman nor docker is installed")
    return subprocess.run([engine, "build", "-t", a.tag, str(dest)]).returncode


if __name__ == "__main__":
    sys.exit(main())
