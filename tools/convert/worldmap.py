"""The map on the bottom screen: a top-down picture of the level's land, one pixel per terrain
vertex (128 units), coloured by each spot's land texture and vertex colour, shaded by the
slope, with the sea in blue. Written as data/map.t3x plus a "map" entry in game.json. Beside it, a 2x-detail copy cut into
2x2 tiles (map_d0.t3x ..) that the map screen loads only where it is zoomed in past the base picture.
"""
import io

import numpy as np
from PIL import Image

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
    units = QUAD
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
    entry = {"file": "map.t3x", "origin": [float(terrain.origin[0]), float(terrain.origin[1])],
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
