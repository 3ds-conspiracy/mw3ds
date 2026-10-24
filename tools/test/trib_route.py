"""Door route between two interior cells of the Tribunal data (for the Mournhold uber cases).

  MW3DS_TRIBUNAL=1 python3.13 tools/test/trib_route.py "Mournhold, Godsreach" "Old Mournhold: Bazaar Sewers"

Prints the shortest chain (--avoid <text> skips cells whose name has that text) of cells by doors (a DOORTO per hop). Names case free; `--list <text>` lists matching cells.
"""
import collections
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
os.environ.setdefault("MW3DS_TRIBUNAL", "1")
from mwfiles import cell_refs, read_game  # noqa: E402


def main():
    args = sys.argv[1:]
    avoid = []
    while "--avoid" in args:
        k = args.index("--avoid")
        avoid.append(args[k + 1].lower())
        del args[k:k + 2]
    edges = collections.defaultdict(list)
    names = {}
    for r in read_game({"CELL"}):
        cn = ""
        for t, d in r.subs:
            if t == "NAME":
                cn = d.split(b"\0")[0].decode("latin-1")
                break
        if not cn:
            continue
        names[cn.lower()] = cn
        for ref in cell_refs(r):
            dc = ref.get("dest_cell")
            if dc:
                edges[cn.lower()].append((dc.lower(), ref["id"], tuple(round(v) for v in ref["pos"])))
    if args and args[0] == "--list":
        for n in sorted(names):
            if args[1].lower() in n:
                print(names[n])
        return
    a, b = args[0].lower(), args[1].lower()
    prev = {a: None}
    q = collections.deque([a])
    while q:
        c = q.popleft()
        if c == b:
            break
        for dc, did, pos in edges.get(c, []):
            if dc not in prev and not any(x in dc for x in avoid):
                prev[dc] = (c, did, pos)
                q.append(dc)
    if b not in prev:
        print("no route")
        return
    path = []
    c = b
    while prev[c]:
        path.append((prev[c][0], c, prev[c][1], prev[c][2]))
        c = prev[c][0]
    for f, t, did, pos in reversed(path):
        print(f"{names.get(f, f)} -> {names.get(t, t)}  via {did} at {pos}")


main()
