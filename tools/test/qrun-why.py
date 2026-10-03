"""Why quest openers failed in a sweep: reads build/qrun.json and the per-batch logs, and for each quest
that didn't start, classifies what its log segment shows (speaker not here / disabled / no talk / the
topic had no response / a response but no journal / no check reached).

  python tools/test/qrun-why.py [qrun|srun] [logs dir]      (default qrun, build/qrun-logs)
"""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    args = sys.argv[1:]
    prefix = args.pop(0) if args and args[0] in ("qrun", "srun") else "qrun"
    logs = Path(args[0]) if args else ROOT / "build" / "qrun-logs"
    plan = json.loads((ROOT / "build" / f"{prefix}.json").read_text())
    reasons, examples = Counter(), defaultdict(list)
    for name, entries in plan.items():
        p = logs / f"{name}.log"
        if not p.exists():
            continue
        lines = p.read_text(errors="replace").splitlines()
        # the lines between one CHECK:journal and the next belong to the next quest
        seg, segs = [], defaultdict(list)
        for l in lines:
            seg.append(l)
            m = re.search(r"check: journal (\S+) = (-?\d+)", l)
            if m:
                segs[m.group(1)].append((int(m.group(2)), seg))
                seg = []
        for q, v, skip in entries:
            if skip:
                continue
            key = q.replace(" ", "_").lower()
            got = segs.get(key) or segs.get(q.lower()) or []
            got = got.pop(0) if got else None
            if got and got[0] >= v:
                continue
            if not got:
                why = "never checked (batch stopped)"
            else:
                text = "\n".join(got[1])
                if "not here" in text:
                    why = "speaker not in the loaded cells"
                elif "disabled or dead" in text:
                    why = "speaker disabled or dead"
                elif "(no talk)" in text:
                    why = "activation opened no talk"
                elif "autoinput: topic" in text and not re.search(r"dialogue: \[[^\]]*\] ", text.split("autoinput: topic")[-1]):
                    why = "topic gave no response (filters)"
                else:
                    why = "response given, journal unchanged"
            reasons[why] += 1
            examples[why].append(f"{q}:{v}" if prefix == "srun" else q)
    for why, n in reasons.most_common():
        print(f"{n:4d}  {why}: {', '.join(examples[why][:12])}")


if __name__ == "__main__":
    main()
