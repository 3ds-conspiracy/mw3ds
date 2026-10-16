"""The map on the bottom screen: a top-down picture of the level's land, one pixel per terrain
vertex (128 units), coloured by each spot's land texture and vertex colour, shaded by the
slope, with the sea in blue, and the towns' buildings (the ex_ statics) drawn over it from above: Vivec's cantons
stand in the sea on shallows, which showed as blue squares where the city is. Written as data/map.t3x plus a "map" entry in game.json. Beside it, a 2x-detail copy cut into
2x2 tiles (map_d0.t3x ..) that the map screen loads only where it is zoomed in past the base picture.
"""
import io
import json

import numpy as np
from PIL import Image

from PIL import ImageDraw

from meshes import extract_shapes, ref_matrix
from mwfiles import cell_refs
from nif import Nif
from terrain import CELL_SIZE, QUAD, ltex_path
from textures import OUT, encode_image

MAX_SIZE = 1024                  # largest 3DS texture side
SUN = np.array([-0.5, 0.5, 0.7]) / np.linalg.norm([-0.5, 0.5, 0.7])   # light from the north-west, like paper maps


def texture_colour(arch, path, cache):
    if path not in cache:
        try:
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGB").resize((1, 1), Image.BOX)
            cache[path] = np.array(img.getpixel((0, 0)), dtype=float) / 255.0
        except Exception:
            cache[path] = np.array([0.45, 0.42, 0.3])
    return cache[path]


def pad_pot(img):
    """The picture on the smallest power-of-two canvas (3DS textures), dark where it ends."""
    w, h = img.size
    canvas = Image.new("RGB", (max(1 << (w - 1).bit_length(), 8), max(1 << (h - 1).bit_length(), 8)), (20, 20, 24))
    canvas.paste(img, (0, 0))
    return canvas


def build_detail(img, units):
    """img: the picture at `units` a pixel. Cut into 2x2 tiles, each with a one-pixel margin of its
    neighbour (so linear sampling shows no seam); returns the game.json "detail" entry."""
    w, h = img.size
    cuts_x, cuts_y = [0, (w + 1) // 2, w], [0, (h + 1) // 2, h]
    tiles = []
    for j in range(2):
        for i in range(2):
            x0, x1, y0, y1 = cuts_x[i], cuts_x[i + 1], cuts_y[j], cuts_y[j + 1]
            mx0, my0 = max(x0 - 1, 0), max(y0 - 1, 0)       # where the margin starts
            tile = pad_pot(img.crop((mx0, my0, min(x1 + 1, w), min(y1 + 1, h))))
            assert max(tile.size) <= MAX_SIZE
            name = f"map_d{len(tiles)}.t3x"
            encode_image(tile, OUT / name, "etc1")
            tiles.append({"file": name, "x": x0, "y": y0, "w": x1 - x0, "h": y1 - y0, "ox": x0 - mx0, "oy": y0 - my0,
                          "tex_width": tile.size[0], "tex_height": tile.size[1]})
    return {"units_per_pixel": units, "width": w, "height": h, "tiles": tiles}


MIN_BUILDING = 256.0             # a building's footprint, across, at least (smaller: a statue, a lamp post)


def hull(pts):
    """The convex hull of 2D points (Andrew's monotone chain), counter-clockwise."""
    pts = sorted(set(map(tuple, np.round(pts, 1))))
    if len(pts) < 3:
        return pts
    def half(seq):
        out = []
        for p in seq:
            while len(out) >= 2 and (out[-1][0] - out[-2][0]) * (p[1] - out[-2][1]) - (out[-1][1] - out[-2][1]) * (p[0] - out[-2][0]) <= 0:
                out.pop()
            out.append(p)
        return out
    lower, upper = half(pts), half(reversed(pts))
    return lower[:-1] + upper[:-1]


def building_outlines(db, arch, cache):
    """[(top z, colour, [(x, y)...] hull in world units)] of the towns' buildings: the exterior statics Morrowind
    names ex_ (houses, towers, cantons, walls, docks), each the top-down hull of its model as placed."""
    objs = db["objects"]
    models = {}
    out = []
    for key, cell in db["CELL"].items():
        if not isinstance(key, tuple):
            continue
        for ref in cell_refs(cell):
            rid = ref["id"].lower()
            rec = objs.get(rid)
            if rec is None or rec.tag != "STAT" or not rid.startswith("ex_"):
                continue
            model = (rec.zstr("MODL") or "").lower()
            if not model:
                continue
            if model not in models:
                try:
                    shapes = extract_shapes(arch, Nif(arch.read("meshes\\" + model)), bind_pose=True)
                except Exception:
                    shapes = []
                pts = [s["pos"] for s in shapes if len(s["pos"])]
                tex = next((s["tex"] for s in sorted(shapes, key=lambda s: -len(s["pos"])) if s.get("tex")), None)
                models[model] = (np.concatenate(pts) if pts else None, tex)
            pts, tex = models[model]
            if pts is None:
                continue
            m = ref_matrix(ref)
            world = pts @ m[:3, :3].T + m[:3, 3]
            span = world[:, :2].max(axis=0) - world[:, :2].min(axis=0)
            if span.max() < MIN_BUILDING:
                continue
            colour = texture_colour(arch, tex, cache) if tex else np.array([0.45, 0.42, 0.36])
            out.append((float(world[:, 2].max()), colour, hull(world[:, :2])))
    return out


def draw_buildings(img, outlines, origin, units):
    """The outlines on the picture (north up, `units` a pixel), lowest first so the taller cover them, each a little
    lighter than its texture with a darker edge, so roofs read as roofs over the land."""
    d = ImageDraw.Draw(img)
    rows = img.size[1]
    for top, colour, pts in sorted(outlines, key=lambda o: o[0]):
        if len(pts) < 3:
            continue
        poly = [((x - origin[0]) / units, rows - 1 - (y - origin[1]) / units) for x, y in pts]
        fill = tuple(int(c * 255) for c in np.clip(colour * 1.15 + 0.06, 0, 1))
        edge = tuple(int(c * 255) for c in np.clip(colour * 0.55, 0, 1))
        d.polygon(poly, fill=fill, outline=edge)
    return img


def build_map(db, arch, terrain, labels):
    """labels: [(text, world x, world y)]. Returns the game.json "map" entry, or None."""
    h, n, c = terrain.h, terrain.n, terrain.c
    rows, cols = h.shape
    # Land texture colour per vertex (from the tile it sits in)
    cache = {}
    tiles = terrain.tiles
    ty = np.clip(np.arange(rows) // 4, 0, tiles.shape[0] - 1)
    tx = np.clip(np.arange(cols) // 4, 0, tiles.shape[1] - 1)
    palette = {v: texture_colour(arch, ltex_path(db, arch, v), cache) for v in np.unique(tiles)}
    base = np.zeros((rows, cols, 3))
    for v, col in palette.items():
        base[(tiles[ty][:, tx] == v)] = col
    shade = 0.55 + 0.6 * np.clip(n @ SUN, 0, 1)
    land = np.clip(base * c * 1.6 * shade[..., None], 0, 1)
    depth = np.clip(-h / 1500.0, 0, 1)[..., None]
    sea = np.array([0.16, 0.3, 0.42]) * (1 - 0.5 * depth) + np.array([0.05, 0.1, 0.2]) * 0.5 * depth
    rgb = np.where((h < 0)[..., None], sea * 0.8 + land * 0.2 * (1 - depth), land)

    img = Image.fromarray((rgb[::-1] * 255).astype(np.uint8))       # north up
    outlines = building_outlines(db, arch, cache)
    img = draw_buildings(img, outlines, terrain.origin, QUAD)
    print(f"  map: {len(outlines)} buildings drawn from above", flush=True)
    # The full-detail picture kept beside the data (out/map_base/<data folder>.png, not on the card):
    # tools/convert/maprender.py lays the cells' top-down renders over it and writes the map again
    base = OUT.parent / "map_base"
    base.mkdir(parents=True, exist_ok=True)
    img.save(base / f"{OUT.name}.png")
    (base / f"{OUT.name}.json").write_text(json.dumps({"origin": [float(terrain.origin[0]), float(terrain.origin[1])],
                                                       "units_per_pixel": QUAD}))
    return encode_map(img, terrain.origin, QUAD, labels)


def encode_map(img, origin, units, labels):
    """The map picture (north up, `units` world units a pixel, its bottom-left corner at origin) as map.t3x, halved
    until it fits a texture, and the step before that cut into detail tiles; returns the game.json "map" entry."""
    finer = None                                  # the step before the last: twice the base picture's detail
    while max(img.size) > MAX_SIZE:
        finer = (img, units)
        img = img.resize((img.size[0] // 2, img.size[1] // 2), Image.LANCZOS)
        units *= 2
    w, hgt = img.size
    canvas = pad_pot(img)
    OUT.mkdir(parents=True, exist_ok=True)
    # ETC1 (4 bits a pixel): the map stays in memory for the HUD minimap, 512 KB at most
    encode_image(canvas, OUT / "map.t3x", "etc1")
    entry = {"file": "map.t3x", "origin": [float(origin[0]), float(origin[1])],
            "units_per_pixel": units, "width": w, "height": hgt, "tex_width": canvas.size[0],
            "tex_height": canvas.size[1],
            "labels": [{"text": t, "x": x, "y": y} for t, x, y in labels]}
    if finer:
        entry["detail"] = build_detail(*finer)
    return entry


def town_labels(db, towns):
    out = []
    for t in towns:
        cells = [xy for xy, rec in db["CELL"].items() if isinstance(xy, tuple) and (rec.id or "").lower() == t.lower()]
        if cells:
            out.append((t, (np.mean([x for x, _ in cells]) + 0.5) * CELL_SIZE,
                        (np.mean([y for _, y in cells]) + 0.5) * CELL_SIZE))
    return out
