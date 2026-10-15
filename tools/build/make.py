"""Runs make for the 3DS build and writes mw3ds.3dsx: python tools/build/make.py [target ...]
On Windows it runs inside the MSYS2 devkitPro environment (MW3DS_MSYS2_DIR in .env); elsewhere it calls make
directly, so devkitPro must be installed (DEVKITPRO set)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools


def main():
    return mwtools.make(sys.argv[1:]).returncode


if __name__ == "__main__":
    sys.exit(main())
