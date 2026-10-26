"""Builds the game's own source for the PC, headless (no 3DS, no emulator): build\\native\\mw3ds-native.exe.
Compiles with zig (python -m ziglang) against the stand-in 3DS headers in tools/test/native/stub, and
zlib from its source (fetched once into build/native/zlib).
    python tools/test/native/build.py            # rebuilds what changed
    MW3DS_NATIVE_BUILD=build/native-x python ...   # another build folder (a second session's own build)
    python tools/test/native/build.py --mem      # the memory checker: build\native-mem, 32-bit as the console, its
                                                 # heap capped at the console's (native_heap.cpp; NATIVE_OLD3DS=1: 39 MB)
"""
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).parent
# --mem: the memory checker (native_heap.cpp): 32-bit, so pointers and the game's structs are the console's size, and
# malloc capped at the console's heap
MEM = "--mem" in sys.argv
BUILD = ROOT / os.environ.get("MW3DS_NATIVE_BUILD", "build/native-mem" if MEM else "build/native")
TARGET = ["-target", "x86-windows-gnu"] if MEM else []
ZLIB = BUILD / "zlib"
ZLIB_URL = "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz"
ZLIB_SRC = ["adler32", "compress", "crc32", "deflate", "inffast", "inflate", "inftrees", "trees", "uncompr", "zutil"]
# replaced by native_stubs.cpp (log, linear heap, the dev updater), native_gpu.cpp (screenshots) or main_native.cpp
SKIP = {"main.cpp", "components/debug/debuglog.cpp", "mwrender/screenshotmanager.cpp", "platform/linear.cpp",
        "platform/heaps.cpp"}
# The two builds' entry files: they only define what source/platform/platform.hpp asks of them and call appMain; the
# game itself is source/engine.cpp, compiled into both, so the console and this build can't drift apart
# (check_entry_files)
ENTRY_FILES = ["source/main.cpp", "tools/test/native/main_native.cpp"]
ENTRY_MAX_LINES = 40
ENTRY_FUNCTIONS = {"main", "platformRunOver", "platformFailFast", "platformClearVram"}
# The renderer's drawing entry points get a suffix in the file that defines them: native_gpu.cpp wraps them so they draw
# only with NATIVE_DRAW=1
RENDERER_RENAMES = ["rendererDrawWorld", "rendererDrawMesh", "rendererDrawMeshGlow", "rendererDrawActor", "rendererDrawGlow",
                    "rendererDrawLocalMap", "rendererLocalMap"]
# a definition: the name and its parameters at the start of a line, the body on the next ("{")
RENDERER_DEFINES = re.compile(r"^[A-Za-z_][\w:<>*& ]*\b(?:" + "|".join(RENDERER_RENAMES) + r")\([^;]*\)\s*\n\{", re.M)
# no fused multiply-add: the 3DS's VFP rounds every operation, and a host-CPU build would round some differently
CXXFLAGS = [os.environ.get("MW3DS_NATIVE_OPT", "-O1"), "-std=c++17", "-w", "-fno-strict-aliasing", "-ffp-contract=off", *(["-DNATIVE_NEWLIB_RAND"] if os.environ.get("NATIVE_NEWLIB_RAND") else []), "-I", str(HERE / "stub"), "-I", str(ROOT / "source"),
            "-I", str(ZLIB), "-include", str(HERE / "stub" / "native_compat.h"), *(["-DNATIVE_MEMCAP"] if MEM else [])]


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
    flags = CXXFLAGS if cxx else ["-O2", "-w", "-I", str(ZLIB), "-I", str(ROOT / "source"), "-DNO_FSEEKO"]
    if cxx and src.is_relative_to(ROOT / "source") and RENDERER_DEFINES.search(src.read_text(encoding="utf-8", errors="replace")):
        flags = flags + [f"-D{n}={n}_sw" for n in RENDERER_RENAMES]
    # (the memory checker: the console inflates a cell file as it reads it, ~60 KB; this build has no fopencookie and
    # inflates it whole, so native_heap.cpp wraps zopen to take that copy outside the console's heap)
    if MEM and src.stem == "zfile":
        flags = flags + ["-Dzopen=zopenWhole"]
    if src.stem == "native_gpu":
        flags = flags + ["-O2"]         # the rasterizer: every pixel of both screens, each frame it draws
    cmd = zig("c++" if cxx else "cc", *TARGET, "-c", src.as_posix(), "-o", obj.as_posix(), *flags)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return None if r.returncode == 0 else f"{src.name}:\n{r.stderr[:3000]}"


_newest = None


def newest_header():
    global _newest
    if _newest is None:
        _newest = max(p.stat().st_mtime for d in (ROOT / "source", HERE / "stub") for p in d.rglob("*.h*"))
    return _newest


def check_entry_files():
    """Stops the build when an entry file holds more than the platform hooks: the game belongs in source/engine.cpp (or
    another source file), where both builds compile it."""
    problems = []
    for rel in ENTRY_FILES:
        text = (ROOT / rel).read_text(encoding="utf-8")
        lines = text.splitlines()
        if len(lines) > ENTRY_MAX_LINES:
            problems.append(f"{rel}: {len(lines)} lines (at most {ENTRY_MAX_LINES})")
        # function definitions: a name and its parameters at the start of a line, the body on the next ("{")
        for i, ln in enumerate(lines[:-1]):
            m = re.match(r"^[A-Za-z_][\w:<>*& ]*?\b(\w+)\s*\([^;]*\)\s*$", ln)
            if m and lines[i + 1].strip() == "{" and m.group(1) not in ENTRY_FUNCTIONS:
                problems.append(f"{rel}:{i + 1}: defines {m.group(1)}(), not one of platform/platform.hpp's hooks")
    if problems:
        sys.exit("build.py: an entry file holds more than platform/platform.hpp's hooks: move it to source/ so both builds "
                 "compile it (or add a hook to platform/platform.hpp when the builds must differ)\n  " + "\n  ".join(problems))


def main():
    check_entry_files()
    fetch_zlib()
    objdir = BUILD / "obj"
    objdir.mkdir(parents=True, exist_ok=True)
    jobs = []
    src = ROOT / "source"
    for s in sorted(src.rglob("*.c*")):
        rel = s.relative_to(src).as_posix()
        if s.suffix in (".cpp", ".c") and rel not in SKIP:
            obj = objdir / "game" / s.relative_to(src).with_suffix(".o")
            obj.parent.mkdir(parents=True, exist_ok=True)
            jobs.append((s, obj, s.suffix == ".cpp"))
    jobs.append((HERE / "main_native.cpp", objdir / "main_native.o", True))
    jobs.append((HERE / "native_stubs.cpp", objdir / "native_stubs.o", True))
    jobs.append((HERE / "native_gpu.cpp", objdir / "native_gpu.o", True))
    if MEM:
        jobs.append((HERE / "native_heap.cpp", objdir / "native_heap.o", True))
    for z in ZLIB_SRC:
        jobs.append((ZLIB / (z + ".c"), objdir / ("z_" + z + ".o"), False))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        errors = [e for e in pool.map(lambda j: compile_one(*j), jobs) if e]
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    exe = BUILD / "mw3ds-native.exe"
    r = subprocess.run(zig("c++", *TARGET, *[o.as_posix() for _, o, _ in jobs], *(["-lkernel32"] if MEM else []), "-o",
                           exe.as_posix()), capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:6000])
        sys.exit(1)
    print("built", exe)


if __name__ == "__main__":
    main()
