"""Builds the game's own source for the PC, headless (no 3DS, no emulator): build\\native\\mw3ds-native.exe.
Compiles with zig (python -m ziglang) against the stand-in 3DS headers in tools/test/native/stub, and
zlib from its source (fetched once into build/native/zlib).
    python tools/test/native/build.py            # rebuilds what changed
"""
import io
import os
import subprocess
import sys
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).parent
BUILD = ROOT / "build" / "native"
ZLIB = BUILD / "zlib"
ZLIB_URL = "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz"
ZLIB_SRC = ["adler32", "compress", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees", "uncompr", "zutil"]
# replaced by native_stubs.cpp (log, linear heap, the dev updater), native_gpu.cpp (screenshots) or main_native.cpp
SKIP = {"main", "log", "devupdate", "screenshot", "linear"}
# renderer.cpp's drawing entry points get a suffix: native_gpu.cpp wraps them so they draw only with NATIVE_DRAW=1
RENDERER_RENAMES = ["rendererDrawWorld", "rendererDrawMesh", "rendererDrawActor", "rendererDrawGlow", "rendererDrawLocalMap",
                    "rendererLocalMap"]
# no fused multiply-add: the 3DS's VFP rounds every operation, and a host-CPU build would round some differently
CXXFLAGS = ["-O1", "-std=c++17", "-w", "-fno-strict-aliasing", "-ffp-contract=off", *(["-DNATIVE_NEWLIB_RAND"] if os.environ.get("NATIVE_NEWLIB_RAND") else []), "-I", str(HERE / "stub"), "-I", str(ROOT / "include"),
            "-I", str(ZLIB), "-include", str(HERE / "stub" / "native_compat.h")]


def zig(*args):
    return [sys.executable, "-m", "ziglang", *args]


def fetch_zlib():
    if (ZLIB / "zlib.h").exists():
        return
    ZLIB.mkdir(parents=True, exist_ok=True)
    data = urllib.request.urlopen(ZLIB_URL, timeout=60).read()
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():
            name = m.name.split("/", 1)[-1]
            if m.isfile() and "/" not in name and name.endswith((".c", ".h")):
                (ZLIB / name).write_bytes(t.extractfile(m).read())


def compile_one(src, obj, cxx):
    if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, newest_header()):
        return None
    flags = CXXFLAGS if cxx else ["-O2", "-w", "-I", str(ZLIB), "-I", str(ROOT / "include"), "-DNO_FSEEKO"]
    if src.stem == "renderer":
        flags = flags + [f"-D{n}={n}_sw" for n in RENDERER_RENAMES]
    if src.stem == "native_gpu":
        flags = flags + ["-O2"]         # the rasterizer: every pixel of both screens, each frame it draws
    cmd = zig("c++" if cxx else "cc", "-c", src.as_posix(), "-o", obj.as_posix(), *flags)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return None if r.returncode == 0 else f"{src.name}:\n{r.stderr[:3000]}"


_newest = None


def newest_header():
    global _newest
    if _newest is None:
        _newest = max(p.stat().st_mtime for d in (ROOT / "include", HERE / "stub") for p in d.rglob("*.h"))
    return _newest


def main():
    fetch_zlib()
    objdir = BUILD / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    jobs = []
    for s in sorted((ROOT / "source").glob("*.cpp")):
        if s.stem not in SKIP:
            jobs.append((s, objdir / (s.stem + ".o"), True))
    jobs.append((HERE / "main_native.cpp", objdir / "main_native.o", True))
    jobs.append((HERE / "native_stubs.cpp", objdir / "native_stubs.o", True))
    jobs.append((HERE / "native_gpu.cpp", objdir / "native_gpu.o", True))
    jobs.append((ROOT / "source" / "cJSON.c", objdir / "cJSON.o", False))
    for z in ZLIB_SRC:
        jobs.append((ZLIB / (z + ".c"), objdir / ("z_" + z + ".o"), False))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        errors = [e for e in pool.map(lambda j: compile_one(*j), jobs) if e]
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    exe = BUILD / "mw3ds-native.exe"
    r = subprocess.run(zig("c++", *[o.as_posix() for _, o, _ in jobs], "-o", exe.as_posix()), capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:6000])
        sys.exit(1)
    print("built", exe)


if __name__ == "__main__":
    main()
