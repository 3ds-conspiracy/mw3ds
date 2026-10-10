"""Writes tools/test/native/main_native.cpp from source/main.cpp: the same file with the native test build's three
changes (the run ends after the scripted input, MW3DS_FAILFAST, the video file closed on exit).
    python tools/test/native/sync_main.py
Run it after every change to source/main.cpp; it stops with a message when main.cpp moved under one of the changes.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "source" / "main.cpp"
OUT = Path(__file__).parent / "main_native.cpp"

PATCHES = [
    ("int main()\n{",
     "void nativeVideoClose();\nint main()\n{"),
    ("\t\t\t\t\tautoShotNow = true;\n\t\t\t\t\tsession->save();\n",
     "\t\t\t\t\tautoShotNow = true;\n\t\t\t\t\tsession->save();\n"
     "\t\t\t\t\tlogExit();       // native test build: the run is over\n"
     "\t\t\t\t\tnativeVideoClose();\n"
     "\t\t\t\t\texit(0);\n"),
    ("\t\t// MW3DS_FAILFAST=1 (native test runs): the run ends at its first failure; the device build carries on\n",
     "\t\t// MW3DS_FAILFAST=1: the run ends at its first failure (after its screenshot): a fix is tried on that, not on the\n"
     "\t\t// failures it drags after it, each a step running to its limit\n"
     "\t\telse if (g_testFailed && getenv(\"MW3DS_FAILFAST\"))\n"
     "\t\t{\n"
     "\t\t\tlogf(\"autoinput: stopped at the first failure (MW3DS_FAILFAST)\");\n"
     "\t\t\tlogExit();\n"
     "\t\t\tnativeVideoClose();\n"
     "\t\t\texit(0);\n"
     "\t\t}\n"),
]


def main():
    raw = SRC.read_bytes().decode("utf-8")
    nl = "\r\n" if "\r\n" in raw else "\n"
    text = raw.replace("\r\n", "\n")
    for old, new in PATCHES:
        if text.count(old) != 1:
            sys.exit(f"sync_main: source/main.cpp no longer has (exactly once):\n{old}")
        text = text.replace(old, new)
    OUT.write_bytes(text.replace("\n", nl).encode("utf-8"))
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
