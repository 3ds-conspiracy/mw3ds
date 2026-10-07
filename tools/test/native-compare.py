"""Compares two native suite runs (run-native-all.ps1 output folders): which cases went from PASS to FAIL
(regressions), which were fixed, which are new or gone.
    python tools/test/native-compare.py build/native-results/<before> build/native-results/<after>
Exits 1 when anything regressed.
"""
import csv
import sys
from pathlib import Path


def load(folder):
    with open(Path(folder) / "results.csv", newline="", encoding="utf-8-sig") as f:
        return {r["case"]: r for r in csv.DictReader(f)}


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    a, b = load(sys.argv[1]), load(sys.argv[2])
    regressed = sorted(c for c in a.keys() & b.keys() if a[c]["result"] == "PASS" and b[c]["result"] != "PASS")
    fixed = sorted(c for c in a.keys() & b.keys() if a[c]["result"] != "PASS" and b[c]["result"] == "PASS")
    passed = sum(r["result"] == "PASS" for r in b.values())
    print(f"after: {passed} of {len(b)} passed (before: {sum(r['result'] == 'PASS' for r in a.values())} of {len(a)})")
    for c in regressed:
        print(f"REGRESSED {c}: {b[c]['detail']}")
    for c in fixed:
        print(f"fixed     {c}")
    for c in sorted(b.keys() - a.keys()):
        print(f"new       {c}: {b[c]['result']}")
    for c in sorted(a.keys() - b.keys()):
        print(f"gone      {c}")
    sys.exit(1 if regressed else 0)


if __name__ == "__main__":
    main()
