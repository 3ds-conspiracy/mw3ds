"""Round trip of a save between the engine's format and Morrowind's (.ess): engine save -> .ess (ess_export.py)
-> engine save (ess.py), a few times, and what changed on the way.

    python tools/test/ess_roundtrip.py <save.json> --data out/world --template <a .ess> [--trips 3] [--work <folder>]

Every trip must give back the save it started from, field by field (the PC format's gaps are filled from the
.mw3ds side file ess_export.py writes beside each .ess). Prints what differs after each trip; exits 1 when
anything does.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "convert"))
import mwtools  # noqa: F401  (.env settings)
import esscells

from ess import same  # noqa: E402  (equal to a float's precision, as ess.py's merge judges it)


def parts(save):
    """The whole save as comparable pieces: every top-level field, and the objects and scripts by what names them
    (the order they're listed in, and whether a cell's objects were kept apart (version 2), don't count)."""
    out = {k: v for k, v in save.items() if k not in ("version", "refs", "scripts")}
    out["refs"] = {(r["c"], r["i"]): r for r in save.get("refs", [])}
    out["scripts"] = {(s["name"].lower(), s.get("ref_c", ""), s.get("ref", -1), s.get("item", "")): s
                      for s in save.get("scripts", [])}
    return out


def diff(a, b):
    """What differs between two parts() results: name -> short description."""
    out = {}
    for k in sorted(set(a) | set(b)):
        x, y = a.get(k), b.get(k)
        if k in ("refs", "scripts"):
            lost = [i for i in x if i not in y]
            added = [i for i in y if i not in x]
            changed = [i for i in x if i in y and not same(x[i], y[i])]
            if lost or added or changed:
                e = changed[0] if changed else None
                out[k] = f"{len(lost)} lost, {len(added)} added, {len(changed)} changed" + (
                    f" (e.g. {e}: {x[e]} -> {y[e]})" if e else f" (e.g. lost {lost[0]})" if lost else
                    f" (e.g. added {added[0]})")
        elif not same(x, y):
            out[k] = f"{str(x)[:150]} -> {str(y)[:150]}"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("save", type=Path)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--template", type=Path, required=True)
    ap.add_argument("--trips", type=int, default=3)
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "ess-roundtrip")
    args = ap.parse_args()
    py = sys.executable
    cur = args.save
    seen = [parts(esscells.read_engine_save(cur))]
    for trip in range(1, args.trips + 1):
        d = args.work / f"trip{trip}"
        for cmd in ([py, str(ROOT / "tools/convert/ess_export.py"), str(cur), "--data", str(args.data), "--template",
                     str(args.template), "--out", str(d)],
                    [py, str(ROOT / "tools/convert/ess.py"), str(d / (cur.stem + ".ess")), "--data", str(args.data),
                     "--out", str(d)]):
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                print(r.stdout + r.stderr)
                return 1
        cur = d / "3ds" / "mw3ds" / "saves" / f"pc_{cur.stem.lower()}.json"
        seen.append(parts(esscells.read_engine_save(cur)))
    unstable = 0
    for t in range(1, len(seen)):
        later = diff(seen[t - 1], seen[t])
        print(f"trip {t}: " + ("the same save" if not later else "differs: " + "; ".join(
            f"{k}: {v}" for k, v in later.items())))
        unstable += bool(later)
    return 1 if unstable else 0


if __name__ == "__main__":
    sys.exit(main())
