"""One quest from a sweep: its opener's conditions and what the log shows around it.
  python tools/test/qrun-show.py <quest>[:stage] [logs dir]   (a stage: the stage sweep's srun batches)"""
import json
import re
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from mwfiles import load_db
from questcheck import JOURNAL_SET
from questrun import conds

ROOT = Path(__file__).resolve().parents[2]


def main():
    q, _, stage = sys.argv[1].lower().partition(":")
    prefix = "srun" if stage else "qrun"
    logs = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build" / "qrun-logs"
    db = load_db()
    for dial in db["dialogue"]:
        for info in dial["infos"]:
            for jq, v in JOURNAL_SET.findall(info.zstr("BNAM") or ""):
                if jq.lower() == q and (not stage or v == stage):
                    print(f"[{dial['name']}] by {info.zstr('ONAM')} sets {v}; pcfac {info.zstr('DNAM')} "
                          f"data {list(info.get('DATA') or b'')}")
                    print("   conds:", conds(info))
                    print("   text:", (info.zstr("NAME") or "")[:120].encode("ascii", "replace").decode())
    plan = json.loads((ROOT / "build" / f"{prefix}.json").read_text())
    for name, entries in plan.items():
        mine = [e for e in entries if e[0] == q and not e[2]]
        if not mine:
            continue
        # which of this quest's checks in the batch (the stage sweep has several)
        nth = next((k for k, e in enumerate(mine) if not stage or str(e[1]) == stage), None)
        if nth is None:
            continue
        lines = (logs / f"{name}.log").read_text(errors="replace").splitlines()
        idx = [i for i, l in enumerate(lines) if f"check: journal {q}" in l]
        if len(idx) <= nth:
            print("not checked in", name)
            continue
        if True:
            i = idx[nth]
            start = i
            while start > 0 and "check: journal" not in lines[start - 1]:
                start -= 1
            for l in lines[start:i + 1]:
                if re.search(r"dialogue:|talk|topic|choice|test:|check:|journal|script", l):
                    print("  ", l[:200])


if __name__ == "__main__":
    main()
