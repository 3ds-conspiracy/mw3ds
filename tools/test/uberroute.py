"""Walking routes across Vvardenfell for the uber quest tests, printed as WALKTO:@x,y,z legs a few hundred units apart:
for legs a follower must walk (an escort), where flying or Recall would leave them behind.

First A* over every exterior path grid point in Morrowind.esm, cells joined at their borders as the engine joins
them (points within 350 across a border, less than 120 apart in height: World::loadCell). Path grids only cover
towns and some roads, so where they don't join: one graph of the path grids in the town cells (streets, bridges,
stairs) and the land's height map (LAND, a 256-unit grid) in the cells without one, joined where they meet; no slope
steeper than --slope, water dearer (swimming). The big rocks and emperor parasols placed on the land (OBSTACLES) are kept clear of:
the land points under them are left out and the legs are only joined where the straight line between them misses
them (an escort walks straight at the player and stops at a rock the player stepped over). Trees and walls are not
in it: the walker's stuck recovery goes round small ones.

  python tools/test/uberroute.py [--step 600] [--secs 60] [--slope 0.9] [--climb 1] [--no-rocks] [--via x,y,z]... -- <x,y,z> <x,y,z>
"""
import argparse
import heapq
import math
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
from mwfiles import load_db, cell_refs  # noqa: E402
from terrain import land_heights  # noqa: E402

CELL = 8192.0


def grid_points(db):
    """[(x, y, z)] and {point: [neighbours]} for every exterior path grid, world coordinates."""
    pts, links, bycell = [], defaultdict(list), {}
    for key, (points, edges) in db["PGRD"].items():
        if not isinstance(key, tuple):
            continue
        base = len(pts)
        ox, oy = key[0] * CELL, key[1] * CELL
        pts += [(x + ox, y + oy, z) for x, y, z in points]
        for a, b in edges:
            links[base + a].append(base + b)
            links[base + b].append(base + a)
        bycell[key] = range(base, len(pts))
    # the borders: close points in neighbouring cells
    for (gx, gy), mine in bycell.items():
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                other = bycell.get((gx + dx, gy + dy))
                if not other or (dx, dy) == (0, 0):
                    continue
                for a in mine:
                    pa = pts[a]
                    for b in other:
                        pb = pts[b]
                        if (pa[0] - pb[0]) ** 2 + (pa[1] - pb[1]) ** 2 <= 350 ** 2 and abs(pa[2] - pb[2]) <= 120:
                            links[a].append(b)
    return pts, links


def nearest(pts, p):
    return min(range(len(pts)), key=lambda i: (pts[i][0] - p[0]) ** 2 + (pts[i][1] - p[1]) ** 2 + 4 * (pts[i][2] - p[2]) ** 2)


def route(pts, links, a, b):
    start, goal = nearest(pts, a), nearest(pts, b)
    dist = lambda i, j: math.dist(pts[i], pts[j])
    cost, came, todo = {start: 0.0}, {}, [(dist(start, goal), start)]
    while todo:
        _, cur = heapq.heappop(todo)
        if cur == goal:
            break
        for nb in links[cur]:
            c = cost[cur] + dist(cur, nb)
            if c < cost.get(nb, 1e18):
                cost[nb], came[nb] = c, cur
                heapq.heappush(todo, (c + dist(nb, goal), nb))
    if goal not in cost:
        return None
    out = [goal]
    while out[-1] != start:
        out.append(came[out[-1]])
    return [pts[i] for i in reversed(out)], cost[goal]


# what an escort cannot walk round, by id prefix: radius at scale 1 (a terrain_rock_* is about 400 across; an emperor
# parasol's stem about 250, its origin high up it; a tree's trunk, with room to pass)
OBSTACLES = (("terrain_rock", 220.0), ("flora_emp_parasol", 160.0), ("flora_tree", 90.0))


def rocks_near(db, a, b):
    """[(x, y, z, r)]: the big rocks in the cells round the leg a -> b."""
    gx0, gx1 = int(math.floor(min(a[0], b[0]) / CELL)) - 2, int(math.floor(max(a[0], b[0]) / CELL)) + 2
    gy0, gy1 = int(math.floor(min(a[1], b[1]) / CELL)) - 2, int(math.floor(max(a[1], b[1]) / CELL)) + 2
    out = []
    for key, c in db["CELL"].items():
        if isinstance(key, tuple) and gx0 <= key[0] <= gx1 and gy0 <= key[1] <= gy1:
            for r in cell_refs(c):
                i = (r["id"] or "").lower()
                for prefix, radius in OBSTACLES:
                    if i.startswith(prefix):
                        out.append((*r["pos"], radius * r["scale"]))
    return out


def blocked(rocks, p, q=None):
    """True when the point p (or the line p -> q) passes within a rock's radius, near its height."""
    for x, y, z, r in rocks:
        if q is None:
            if math.hypot(p[0] - x, p[1] - y) < r and abs(p[2] - z) < 1000:
                return True
            continue
        dx, dy = q[0] - p[0], q[1] - p[1]
        l2 = dx * dx + dy * dy
        t = 0.0 if l2 < 1 else max(0.0, min(1.0, ((x - p[0]) * dx + (y - p[1]) * dy) / l2))
        cx, cy, cz = p[0] + t * dx, p[1] + t * dy, p[2] + t * (q[2] - p[2])
        if math.hypot(cx - x, cy - y) < r and abs(cz - z) < 1000:
            return True
    return False


def combined_route(db, a, b, slope, spacing=256.0, climb=1.0, rocks=()):
    """A* over one graph: the path grids where a cell has one (towns: streets, bridges, stairs) and the height map's
    256-unit grid in the cells without (the wilds), joined where they meet. [(x, y, z)], length, or None."""
    gx0, gx1 = int(math.floor(min(a[0], b[0]) / CELL)) - 2, int(math.floor(max(a[0], b[0]) / CELL)) + 2
    gy0, gy1 = int(math.floor(min(a[1], b[1]) / CELL)) - 2, int(math.floor(max(a[1], b[1]) / CELL)) + 2
    heights = {}

    def h(x, y):
        cx, cy = int(math.floor(x / CELL)), int(math.floor(y / CELL))
        if (cx, cy) not in heights:
            heights[cx, cy] = land_heights(db["LAND"].get((cx, cy)))
        i = min(64, max(0, round((x - cx * CELL) / 128.0)))
        j = min(64, max(0, round((y - cy * CELL) / 128.0)))
        return float(heights[cx, cy][j][i])

    town = {k for k, v in db["PGRD"].items() if isinstance(k, tuple) and len(v[0]) >= 20
            and gx0 <= k[0] <= gx1 and gy0 <= k[1] <= gy1}
    cell_of = lambda x, y: (int(math.floor(x / CELL)), int(math.floor(y / CELL)))
    # nodes: ("g", i) path grid points, ("t", ix, iy) land grid points outside the towns
    pts, links = grid_points(db)
    gpos = {("g", i): pts[i] for i in range(len(pts)) if cell_of(*pts[i][:2]) in town}
    x0, y0 = gx0 * CELL, gy0 * CELL
    nx, ny = int((gx1 - gx0 + 1) * CELL / spacing), int((gy1 - gy0 + 1) * CELL / spacing)
    tpos = {}

    def tnode(ix, iy):
        n = ("t", ix, iy)
        if n not in tpos:
            x, y = x0 + ix * spacing, y0 + iy * spacing
            tpos[n] = (x, y, max(h(x, y), -400.0))      # (also in the towns' cells: their grids leave gaps; dearer there)
        return tpos[n]

    def where(n):
        return gpos[n] if n[0] == "g" else tnode(n[1], n[2])

    # path grid points near the wilds: each joins the land points round it (a town's edge, its gates)
    edge = defaultdict(list)
    for n, (x, y, z) in gpos.items():
        ix, iy = round((x - x0) / spacing), round((y - y0) / spacing)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                t = tnode(ix + dx, iy + dy) if 0 <= ix + dx < nx and 0 <= iy + dy < ny else None
                if t and abs(t[2] - z) < 150:
                    edge[n].append(("t", ix + dx, iy + dy))
                    edge[("t", ix + dx, iy + dy)].append(n)

    def neighbours(n):
        if n[0] == "g":
            for m in links[n[1]]:
                if ("g", m) in gpos:
                    yield ("g", m)
        else:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    ix, iy = n[1] + dx, n[2] + dy
                    if (dx, dy) != (0, 0) and 0 <= ix < nx and 0 <= iy < ny and (("t", ix, iy) in ends or not blocked(rocks, tnode(ix, iy))):
                        yield ("t", ix, iy)
        yield from edge.get(n, ())

    def nearest_node(p):
        best, bd = None, 1e18
        for n, q in list(gpos.items()):
            d = (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 + 4 * (q[2] - p[2]) ** 2
            if d < bd:
                best, bd = n, d
        ix, iy = round((p[0] - x0) / spacing), round((p[1] - y0) / spacing)
        t = tnode(ix, iy) if 0 <= ix < nx and 0 <= iy < ny else None
        if t and (t[0] - p[0]) ** 2 + (t[1] - p[1]) ** 2 + 4 * (t[2] - p[2]) ** 2 < bd:
            best = ("t", ix, iy)
        return best

    start, goal = nearest_node(a), nearest_node(b)
    ends = {start, goal}             # (these two whatever rocks stand by them)
    gq = where(goal)
    cost, came, todo = {start: 0.0}, {}, [(0.0, start)]
    while todo:
        _, cur = heapq.heappop(todo)
        if cur == goal:
            break
        pc = where(cur)
        for nb in neighbours(cur):
            pn = where(nb)
            run = math.hypot(pn[0] - pc[0], pn[1] - pc[1])
            if run < 1:
                continue
            if cur[0] == "t" and nb[0] == "t" and abs(pn[2] - pc[2]) / run > slope and max(pn[2], pc[2]) > 0:
                continue                                    # too steep to walk (under the water: swim)
            c = cost[cur] + run * (4.0 if pn[2] < 0 else 1.0) + climb * abs(pn[2] - pc[2])
            if (cur[0] == "t" and cell_of(pc[0], pc[1]) in town) or (nb[0] == "t" and cell_of(pn[0], pn[1]) in town):
                c += run * 2.0                                # land inside a town's cell: its grid is the better way
            if c < cost.get(nb, 1e18):
                cost[nb], came[nb] = c, cur
                heapq.heappush(todo, (c + math.hypot(pn[0] - gq[0], pn[1] - gq[1]), nb))
    if goal not in cost:
        # where the search got to: the node it reached nearest the goal (a wall of slope, or a gap between graphs)
        near = min(cost, key=lambda n: math.hypot(where(n)[0] - gq[0], where(n)[1] - gq[1]))
        print("no route: %d nodes reached, nearest the goal %s at %.0f, %.0f, %.0f (%.0f away)" % (
            len(cost), near, *where(near), math.hypot(where(near)[0] - gq[0], where(near)[1] - gq[1])), file=sys.stderr)
        return None
    out = [goal]
    while out[-1] != start:
        out.append(came[out[-1]])
    path = [tuple(where(n)) for n in reversed(out)]
    path = [(x, y, max(z, 0.0)) for x, y, z in path]
    return [a] + path[1:-1] + [b], cost[goal]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start")
    ap.add_argument("goal")
    ap.add_argument("--step", type=float, default=600.0, help="leave out points closer than this to the last kept one")
    ap.add_argument("--secs", type=int, default=60, help="the seconds each WALKTO step may take")
    ap.add_argument("--terrain", action="store_true", help="skip the path-grid-only try: the combined graph at once")
    ap.add_argument("--slope", type=float, default=0.9, help="steepest walkable rise per unit (0.9: 42 degrees)")
    ap.add_argument("--climb", type=float, default=1.0, help="cost of a unit up or down against one along (escorts: 4)")
    ap.add_argument("--no-rocks", action="store_true", help="walk over the big rocks as v1 routes did")
    ap.add_argument("--via", action="append", default=[], help="x,y,z to pass through (more than one: in order)")
    a = ap.parse_args()
    stops = [tuple(float(v) for v in t.split(",")) for t in [a.start, *a.via, a.goal]]
    db = load_db()
    pts, links = grid_points(db)
    path, length, rocks = [], 0.0, []
    for p0, p1 in zip(stops, stops[1:]):
        legrocks = [] if a.no_rocks else rocks_near(db, p0, p1)
        rocks += legrocks
        r = None if a.terrain else route(pts, links, p0, p1)
        if not r:
            r = combined_route(db, p0, p1, a.slope, climb=a.climb, rocks=legrocks)
        if not r:
            sys.exit(f"no route between {p0} and {p1} (path grids or land)")
        path += r[0] if not path else r[0][1:]
        length += r[1]
    # every --step units, and sooner where the straight line on from the last kept point would cross a rock
    kept = [path[0]]
    for i, p in enumerate(path[1:-1], 1):
        if math.dist(p, kept[-1]) >= a.step or (rocks and blocked(rocks, kept[-1], path[i + 1])):
            kept.append(p)
    kept.append(path[-1])
    print(f"# route {a.start} -> {a.goal}: {len(path)} path points, {length:.0f} units, {len(kept)} legs", file=sys.stderr)
    for x, y, z in kept:
        print(f"{a.secs} 0 0 0 0 WALKTO:@{x:.0f},{y:.0f},{z:.0f}")


if __name__ == "__main__":
    main()
