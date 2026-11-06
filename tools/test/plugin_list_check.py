"""Checks the converter window's plugin list (tools/release/wizard.py) against what the converter accepts
(tools/convert/mwfiles.py load_order), with small plugin files holding only a header. No game data is needed.

    python tools/test/plugin_list_check.py

Prints one line per check and "RESULT: PASS" or "RESULT: FAIL (n failed)"; exit status 1 on a failure.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import wizard  # noqa: E402

failures = []


def check(ok, what):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures.append(what)


def plugin(path, need):
    """A record file holding a TES3 header that lists those masters."""
    subs = b"HEDR" + struct.pack("<I", 300) + struct.pack("<fI32s256sI", 1.3, 0, b"", b"", 0)
    for m in need:
        name = m.encode() + b"\0"
        subs += b"MAST" + struct.pack("<I", len(name)) + name + b"DATA" + struct.pack("<IQ", 8, 0)
    path.write_bytes(b"TES3" + struct.pack("<III", len(subs), 0, 0) + subs)


# What the converter says about a load order, run on its own so its settings are read fresh
ACCEPTS = """
import sys
sys.path.insert(0, sys.argv[1])
import mwfiles
print("|".join(p.name for p in mwfiles.load_order()))
"""


def converter_order(folder, plugins, tribunal, bloodmoon):
    env = dict(os.environ, MW3DS_MORROWIND_DIR=str(folder), MW3DS_PLUGINS=";".join(plugins), MW3DS_OVERLAY_DIRS="",
               MW3DS_TRIBUNAL="1" if tribunal else "", MW3DS_BLOODMOON="1" if bloodmoon else "")
    r = subprocess.run([sys.executable, "-c", ACCEPTS, str(ROOT / "tools" / "convert")], capture_output=True, text=True,
                       env=env)
    return r.stdout.strip() if r.returncode == 0 else None


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        data = folder / "Data Files"
        data.mkdir()
        for esm in ("Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm"):
            plugin(data / esm, [])
        plugin(data / "Base Only.esp", ["Morrowind.esm"])
        plugin(data / "Needs Tribunal.esp", ["Morrowind.esm", "Tribunal.esm"])
        plugin(data / "Needs Bloodmoon.esp", ["Morrowind.esm", "Bloodmoon.esm"])
        plugin(data / "Needs Both.esp", ["Morrowind.esm", "Tribunal.esm", "Bloodmoon.esm"])
        plugin(data / "Needs Other.esp", ["Morrowind.esm", "Needs Both.esp"])
        plugin(data / "Needs Absent.esp", ["Morrowind.esm", "Not There.esp"])
        names = lambda t, b: {n: r for n, r, _ in wizard.plugin_files(folder, t, b)[0]}

        off = names(False, False)
        check(off["Base Only.esp"] is None, "no add-ons: a plugin of the base game can be picked")
        check("Add Tribunal" in (off["Needs Tribunal.esp"] or ""), "no add-ons: a Tribunal plugin says to tick Add Tribunal")
        check("Add Tribunal" in (off["Needs Both.esp"] or "") and "Add Bloodmoon" in off["Needs Both.esp"],
              "no add-ons: one needing both names both boxes")
        check(bool(off["Needs Other.esp"]), "no add-ons: one needing a refused plugin is refused")
        check("not in Data Files" in (off["Needs Absent.esp"] or ""), "a master that is not there says so")

        one = names(True, False)
        check(one["Needs Tribunal.esp"] is None, "Tribunal ticked: its plugin can be picked")
        check("Add Bloodmoon" in (one["Needs Both.esp"] or "") and "Add Tribunal" not in one["Needs Both.esp"],
              "Tribunal ticked: one needing both asks only for Bloodmoon")

        both = names(True, True)
        for n in ("Needs Tribunal.esp", "Needs Bloodmoon.esp", "Needs Both.esp", "Needs Other.esp"):
            check(both[n] is None, f"both ticked: {n} can be picked")
        check(bool(both["Needs Absent.esp"]), "both ticked: a missing master still refuses")

        rows, needs = wizard.plugin_files(folder, True, True)
        usable = [n for n, r, _ in rows if not r]
        shown = {n: s for n, _, s in rows}
        check(shown["Needs Both.esp"] == ["Tribunal", "Bloodmoon"], "the list shows what a plugin needs")
        order = wizard.plugins_in_order(["Needs Other.esp"], usable, needs)
        check(order == ["Needs Both.esp", "Needs Other.esp"], "picking one brings the plugin it needs, before it")

        # The window and the converter agree on every plugin at every setting
        for t, b in ((False, False), (True, False), (False, True), (True, True)):
            rows, needs = wizard.plugin_files(folder, t, b)
            usable = [n for n, r, _ in rows if not r]
            for n, refusal, _ in rows:
                picked = wizard.plugins_in_order([n], usable, needs) if not refusal else [n]
                got = converter_order(folder, picked, t, b)
                check((got is not None) == (refusal is None),
                      f"tribunal {t}, bloodmoon {b}: {n}: window {'refuses' if refusal else 'allows'}, "
                      f"converter {'accepts' if got else 'refuses'}")
        got = converter_order(folder, ["Needs Both.esp"], True, True)
        check(got == "Morrowind.esm|Tribunal.esm|Bloodmoon.esm|Needs Both.esp", "load order: the add-ons, then the plugin")
    print(f"RESULT: {'PASS' if not failures else f'FAIL ({len(failures)} failed)'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
