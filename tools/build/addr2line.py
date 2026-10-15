"""Code address(es) to function and line in mw3ds.elf: python tools/build/addr2line.py 0x001DC63C [...]"""
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools


def main():
    tool = f"{mwtools.devkitarm_bin()}/arm-none-eabi-addr2line"
    cmd = f"cd {shlex.quote(mwtools.project_dir_for_shell())} && {tool} -f -C -i -e mw3ds.elf {' '.join(shlex.quote(a) for a in sys.argv[1:])}"
    return mwtools.devkitpro_bash(cmd).returncode


if __name__ == "__main__":
    sys.exit(main())
