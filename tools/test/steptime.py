"""Where an uber run's game time goes, from its log.txt and case file.

  python -I steptime.py <log> <case.txt> [--top N] [--verb V]

A step's game seconds: 'took N s' when logged (steps of 15 s and over), else the driver's own
'reached/flew to ... in N s', else (for a driven step that ended early) the frame rate integrated over its
wall-clock span (fps / 30), else its budget. Native logs run at many hundred fps, so the wall clock is
not game time.
"""
import re
import sys
from collections import defaultdict

BLOCKING = {"WALKTO", "DOORTO", "ACTIVATE", "FLYTO", "HOPTO", "KILL", "LOOT", "PICKUP", "USELOCKPICK", "USEPROBE",
            "SEARCH", "TAKE", "TOPIC", "UNSEEN", "TRAVEL", "ESCORT", "TALK", "WAITDOOR", "PUT", "STRIKE", "CASTAT", "UNTIL"}


def parse_case(path):
    steps = []
    for line in open(path, encoding="utf-8", errors="replace"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        f = s.split()
        try:
            secs = float(f[0])
        except ValueError:
            continue
        verb, arg = "", ""
        if len(f) >= 6 and not re.match(r"^-?[0-9.]+$", f[5]):
            verb, _, arg = f[5].partition(":")
        steps.append((secs, verb, arg, s))
    return steps


def main():
    args = sys.argv[1:]
    top = 40
    only = None
    if "--top" in args:
        i = args.index("--top"); top = int(args[i + 1]); del args[i:i + 2]
    if "--verb" in args:
        i = args.index("--verb"); only = args[i + 1]; del args[i:i + 2]
    log, case = args
    steps = parse_case(case)
    starts = {}           # step -> first wall ms
    fps = []              # (ms, fps)
    took = {}
    exact = {}
    notes = defaultdict(list)
    cur = -1
    order = []
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.match(r"\[(\d+)\] (.*)", line.rstrip("\n"))
        if not m:
            continue
        ms, rest = int(m.group(1)), m.group(2)
        m2 = re.match(r"autoinput: step (\d+) \[", rest)
        if m2:
            n = int(m2.group(1))
            if n not in starts:
                starts[n] = ms
                order.append(n)
            cur = n
            continue
        m2 = re.match(r"autoinput: step (\d+) took ([0-9.]+) s", rest)
        if m2:
            took[int(m2.group(1))] = float(m2.group(2))
            continue
        m2 = re.match(r"fps ([0-9.]+)", rest)
        if m2:
            fps.append((ms, float(m2.group(1))))
            continue
        m2 = re.match(r"drive: (?:reached|flew to) .* in ([0-9.]+) s", rest) or re.match(r"drive: until .*: held after ([0-9.]+) s", rest)
        if m2:
            exact[cur] = float(m2.group(1))
        if rest.startswith("autoinput: end"):
            starts[len(steps)] = ms
            continue
        for key in ("drive: stuck at", "drive: route by trial steps:", "drive: FAIL", "monitor:", "combat: swing at",
                    "drive: route to", "drive: FLYTO @", "drive: DOORTO candidate", "world: loading", "expect: FAIL",
                    "combat: player hits", "test: warped", "drive: warp"):
            if rest.startswith(key):
                notes[cur].append(rest)
                break

    # game seconds per wall ms from the once-a-second fps lines
    def sim_between(a, b):
        tot = 0.0
        last_rate = (fps[-1][1] / 30.0 / 1000.0) if fps else 1 / 1000.0
        for ms, f in fps:
            lo, hi = ms - 1000, ms
            o = max(0, min(b, hi) - max(a, lo))
            tot += o * f / 30.0 / 1000.0
        if fps and b > fps[-1][0]:
            tot += (b - max(a, fps[-1][0])) * last_rate
        if fps and a < fps[0][0] - 1000:
            tot += (min(b, fps[0][0] - 1000) - a) * (fps[0][1] / 30.0 / 1000.0)
        return tot

    rows = []
    check = []
    for n in order:
        if n >= len(steps):
            continue
        a = starts[n]
        nxt = n + 1
        while nxt not in starts and nxt <= len(steps):
            nxt += 1
        b = starts.get(nxt, a)
        budget, verb, arg, text = steps[n]
        est = sim_between(a, b)
        if n in took:
            sim, src = took[n], "took"
        elif n in exact:
            sim, src = exact[n], "drive"
        elif verb in BLOCKING:
            sim, src = min(est, budget), "fps"
        else:
            sim, src = budget, "budget"
            check.append((est, budget))
        rows.append(dict(n=n, verb=verb or "(wait)", arg=arg, budget=budget, sim=sim, src=src, wall=(b - a) / 1000.0,
                         notes=notes.get(n, []), text=text))

    total = sum(r["sim"] for r in rows)
    wall = (starts[max(starts)] - starts[min(starts)]) / 1000.0
    print(f"{len(rows)} steps, game time {total:.0f} s ({total / 3600:.2f} h), wall {wall:.0f} s (native)")
    if check:
        ratios = sorted(e / b for e, b in check if b >= 1)
        if ratios:
            print(f"fps estimator check on plain waits >= 1 s: median est/budget {ratios[len(ratios) // 2]:.2f} "
                  f"(n={len(ratios)})")
    print()
    by = defaultdict(lambda: [0, 0.0, 0.0, 0.0])
    for r in rows:
        v = by[r["verb"]]
        v[0] += 1; v[1] += r["sim"]; v[2] += r["budget"]; v[3] += r["wall"]
    print(f"{'verb':12} {'n':>5} {'game s':>8} {'share':>6} {'avg':>6} {'budget':>8} {'wall s':>7}")
    for verb, (n, s, b, w) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f"{verb:12} {n:5d} {s:8.0f} {100 * s / total:5.1f}% {s / n:6.1f} {b:8.0f} {w:7.0f}")
    print()
    stuck = [r for r in rows if any(x.startswith("drive: stuck") or x.startswith("drive: route by trial") for x in r["notes"])]
    print(f"steps with stuck-recovery / trial routes: {len(stuck)}, {sum(r['sim'] for r in stuck):.0f} game s")
    fails = [r for r in rows if any(x.startswith("drive: FAIL") for x in r["notes"])]
    print(f"steps with a drive FAIL: {len(fails)}, {sum(r['sim'] for r in fails):.0f} game s")
    print()
    sel = [r for r in rows if not only or r["verb"] == only]
    sel.sort(key=lambda r: -r["sim"])
    print(f"top {top} steps by game time:")
    for r in sel[:top]:
        flags = []
        c = sum(1 for x in r["notes"] if x.startswith("drive: stuck"))
        if c: flags.append(f"stuck x{c}")
        c = sum(1 for x in r["notes"] if x.startswith("drive: route by trial"))
        if c: flags.append(f"trial x{c}")
        c = sum(1 for x in r["notes"] if x.startswith("drive: route to"))
        if c > 1: flags.append(f"reroute x{c - 1}")
        c = sum(1 for x in r["notes"] if x.startswith("combat: swing"))
        if c: flags.append(f"swings {c}")
        c = sum(1 for x in r["notes"] if x.startswith("combat: player hits"))
        if c: flags.append(f"hits {c}")
        c = sum(1 for x in r["notes"] if x.startswith("world: loading"))
        if c: flags.append(f"cells {c}")
        for x in r["notes"]:
            if x.startswith("drive: FAIL"):
                flags.append(x[7:60])
            if x.startswith("monitor:"):
                flags.append("monitor: " + x[9:50]); break
        print(f"  step {r['n']:5d} {r['sim']:7.1f}s [{r['src']:6}] of {r['budget']:4.0f}  {r['text'][:70]:70}  {'; '.join(flags)}")


if __name__ == "__main__":
    main()
