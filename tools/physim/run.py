"""Builds tools/physim (the game's own player + collision code, host-compiled with zig) and runs it
against a converted cell: python tools/physim/run.py balmora__ra_virr__trader -256 -64 93 [walks] [seconds]"""
import os
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
BUILD = ROOT / "build" / "physim"
# converted data to read (MW3DS_OUT=out/world for the whole island)
DATA = Path(os.environ.get("MW3DS_OUT") or ROOT / "out" / "data")


def build():
    """physim.exe, rebuilt when its sources changed."""
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / "physim.exe"
    srcs = [HERE / "physim.cpp", ROOT / "source" / "player.cpp", ROOT / "source" / "collision.cpp",
            ROOT / "include" / "player.h", ROOT / "include" / "collision.h"]
    if not exe.exists() or max(s.stat().st_mtime for s in srcs) > exe.stat().st_mtime:
        subprocess.check_call([sys.executable, "-m", "ziglang", "c++", "-O2", "-std=c++17", "-w",
                               "-I", str(HERE / "stub"), "-I", str(ROOT / "include"),
                               *(str(s) for s in srcs if s.suffix == ".cpp"), "-o", str(exe)])
    return exe


def cell_file(stem, ext, data=None):
    """A cell's file: cells/<xx>/<stem><ext> (level.py shard_cells), else the older cells/<stem><ext>."""
    import zlib as _z
    base = (data or DATA) / "cells"
    p = base / ("%02x" % (_z.crc32(stem.encode("latin-1")) & 63)) / (stem + ext)
    return p if p.exists() else base / (stem + ext)


def raw_cell(stem):
    """The cell file uncompressed, where physim can read it."""
    data = cell_file(stem, ".cel").read_bytes()
    if data[:4] == b"MWZ1":
        data = zlib.decompress(data[8:])
    raw = BUILD / (stem + ".raw")
    raw.write_bytes(data)
    return raw


def main():
    exe = build()
    sys.exit(subprocess.call([str(exe), str(raw_cell(sys.argv[1])), *sys.argv[2:]]))


if __name__ == "__main__":
    main()
