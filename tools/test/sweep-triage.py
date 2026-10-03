"""Compact view of every failed quest / stage in a sweep, for triage: the setup the test did, what the
speaker said, the topic and choices, and the journal it ended with.
  python tools/test/sweep-triage.py qrun|srun [filter]
"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KEEP = re.compile(r"test: (journal|gave|global|talk|going|join|rep|level|local)|dialogue:|autoinput: (topic|choice)|"
                  r"check:|drive:|was fighting|not here|disabled")


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "qrun"
    filt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    plan = json.loads((ROOT / "build" / f"{prefix}.json").read_text())
    results = (ROOT / "build" / f"{prefix}-results.txt").read_text().splitlines()
    failed = defaultdict(list)                       # batch -> [(quest, stage wanted)]
    batch = []
    for line in results:
        m = re.match(r"\s+FAIL\s+(\S+): wanted (-?\d+)", line)
        if m:
            batch.append((m.group(1), int(m.group(2))))
        m = re.match(r"(\w+-\d+): ", line)
        if m:
            failed[m.group(1)] += batch
            batch = []
    for name, fails in failed.items():
        log = ROOT / "build" / "qrun-logs" / f"{name}.log"
        if not fails or not log.exists():
            continue
        lines = log.read_text(errors="replace").splitlines()
        # segments: the lines up to each check
        segs, seg = defaultdict(list), []
        for l in lines:
            seg.append(l)
            m = re.search(r"check: journal (\S+) = (-?\d+)", l)
            if m:
                segs[m.group(1)].append(seg)
                seg = []
        # which occurrence of the quest each failure is (the stage sweep has several per quest)
        order = defaultdict(int)
        runs = [e for e in plan[name] if not e[2]]
        for q, v, _ in runs:
            key = q.replace(" ", "_").lower()
            k = order[key]
            order[key] += 1
            if (q, v) not in fails and (key, v) not in fails:
                continue
            if filt and not filt.search(q):
                continue
            s = segs.get(key, [])
            print(f"=== {name} {q}:{v}")
            if k < len(s):
                for l in s[k]:
                    if KEEP.search(l):
                        print("   ", re.sub(r"^\[\s*\d+\] ", "", l)[:170])
            else:
                print("    (never checked)")


if __name__ == "__main__":
    main()
