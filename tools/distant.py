"""Distant land: a coarse, pre-lit height grid of every exterior cell, drawn past the 3 x 3 block of
cells the 3DS keeps loaded so the view reaches further than the loaded cells.

One vertex every DISTANT_STEP terrain vertices (1024 units), coloured like the near terrain: the land
texture's average colour x the LAND vertex colour x the same baked sun and ambient. Land under the
sea is flattened to the water surface in the water's colour (the loaded cells' water plane doesn't
reach out there). The grid runs SEA_MARGIN cells past the terrain on every side, open sea.

Big objects (cantons, towers, strongholds, great rocks: STATIC_MIN_SIZE across and up) come along,
simplified by vertex clustering (STATIC_CLUSTER units) and coloured the same way (texture average x
vertex colour x light), one mesh per exterior cell.

Written as data/distant.bin (zlib'd like the cells):
  'MWD1', i32 cell x0, i32 cell y0, u32 cells nx, u32 cells ny, u32 samples per cell side (step count),
  then (ny * step + 1) rows of (nx * step + 1) samples: s16 height, u8 r, g, b  (row y, then x)
  then 'STAT', u32 cells, per cell: i32 gx, i32 gy, u32 num_vertices, u32 num_indices,
  vertices (s16 x, y relative to the cell's corner x 2, s16 z, u8 r, g, b, pad), u16 indices (pad to 4)
"""
import struct

import numpy as np

from terrain import VERTS
from worldmap import texture_colour
from terrain import ltex_path

DISTANT_STEP = 8          # terrain vertices per distant sample (65-vertex cells: 8 quads a side)
SEA_MARGIN = 4            # cells of open sea around the terrain
STATIC_MIN_SIZE = 2000.0  # objects at least this big (bounding box diagonal, scaled) are seen from afar
STATIC_TRIS_PER_UNIT = 1 / 25.0   # each model simplified to about its size / 25 triangles ...
STATIC_TRIS = (30, 300)           # ... within these bounds (a Vivec canton ~280, a big rock ~80)
STATIC_TYPES = ("STAT", "ACTI", "DOOR")


def simplify(pos, col, tris, cell):
    """Vertex clustering: vertices in the same cell-sized cube become one (average position and
    attributes, col: N x k); triangles that collapse go."""
    keys = np.floor(pos / cell).astype(np.int64)
    uniq, inv = np.unique(keys, axis=0, return_inverse=True)
    inv = inv.ravel()
    n = len(uniq)
    cnt = np.bincount(inv, minlength=n).astype(float)[:, None]
    p = np.zeros((n, 3))
    c = np.zeros((n, col.shape[1]))
    np.add.at(p, inv, pos)
    np.add.at(c, inv, col)
    p /= cnt
    c /= cnt
    t = inv[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t = t[keep]
    if len(t):
        _, first = np.unique(np.sort(t, axis=1), axis=0, return_index=True)
        t = t[np.sort(first)]
    # drop vertices nothing uses
    used = np.unique(t) if len(t) else np.zeros(0, dtype=int)
    remap = np.full(n, -1)
    remap[used] = np.arange(len(used))
    return p[used], c[used], remap[t] if len(t) else t


def object_colour(arch, path, cache):
    """A texture's average colour over its opaque texels (leaves and grilles are mostly transparent,
    black where they are)."""
    if path not in cache:
        try:
            import io
            from PIL import Image
            img = np.asarray(Image.open(io.BytesIO(arch.read(path))).convert("RGBA").resize((32, 32)), dtype=float) / 255.0
            a = img[..., 3:4]
            w = a.sum()
            cache[path] = (img[..., :3] * a).sum((0, 1)) / w if w > 1.0 else img[..., :3].mean((0, 1))
        except Exception:
            cache[path] = np.array([0.45, 0.42, 0.35])
    return cache[path]


def distant_statics(db, arch, sun_dir, sun, ambient):
    """{(gx, gy): (positions Nx3 world, colours Nx3, triangles Mx3)} of the big objects in each exterior cell."""
    from meshes import extract_shapes, ref_matrix
    from mwfiles import cell_refs
    from nif import Nif
    objects = db["objects"]
    models, tex_cache = {}, {}

    def model_mesh(model):
        key = model.lower()
        if key in models:
            return models[key]
        out = None
        try:
            nif = Nif(arch.read("meshes\\" + key))
            for r in nif.roots:
                b = nif.get(r)
                if b is not None and "rotation" in b:
                    b["rotation"] = [1, 0, 0, 0, 1, 0, 0, 0, 1]
            P, N, C, T, base = [], [], [], [], 0
            for s in extract_shapes(arch, nif):
                if s.get("skinned") or s["flags"] & 1 or len(s["tris"]) == 0:     # blended: glass, smoke
                    continue
                tex = object_colour(arch, s["tex"], tex_cache) if s.get("tex") else np.array([0.5, 0.5, 0.5])
                col = np.tile(tex, (len(s["pos"]), 1))
                if s.get("vcol") is not None:
                    col = col * np.clip(np.asarray(s["vcol"])[:, :3], 0, 1)
                P.append(s["pos"]); N.append(s["nrm"]); C.append(col)
                T.append(np.asarray(s["tris"]).reshape(-1, 3) + base)
                base += len(s["pos"])
            if P:
                p = np.concatenate(P)
                size = float(np.linalg.norm(p.max(0) - p.min(0)))
                attrs = np.concatenate([np.concatenate(N), np.concatenate(C)], axis=1)
                t = np.concatenate(T)
                # Simplified once per model: coarser clusters until it fits its triangle budget
                budget = int(np.clip(size * STATIC_TRIS_PER_UNIT, *STATIC_TRIS))
                cell = max(size / (np.sqrt(budget) * 1.5), 8.0)
                sp, sa, st = simplify(p, attrs, t, cell)
                while len(st) > budget and cell < size:
                    cell *= 1.3
                    sp, sa, st = simplify(p, attrs, t, cell)
                if len(st):
                    nrm = sa[:, :3] / np.maximum(np.linalg.norm(sa[:, :3], axis=1, keepdims=True), 1e-6)
                    out = (sp, nrm, np.nan_to_num(sa[:, 3:], nan=0.5), st, size)
        except Exception as e:
            out = None
        models[key] = out
        return out

    cells = {}
    for xy, cell in db["CELL"].items():
        if not isinstance(xy, tuple):
            continue
        P, C, T, base = [], [], [], 0
        for ref in cell_refs(cell):
            if ref["deleted"] or not ref["pos"] or not ref["id"]:
                continue
            rec = objects.get(ref["id"].lower())
            if rec is None or rec.tag not in STATIC_TYPES:
                continue
            model = rec.zstr("MODL")
            if not model:
                continue
            mm = model_mesh(model)
            if mm is None or mm[4] * ref["scale"] < STATIC_MIN_SIZE:
                continue
            p, n, c, t, _ = mm
            m = ref_matrix(ref)
            wp = p @ m[:3, :3].T + m[:3, 3]
            wn = n @ m[:3, :3].T
            wn /= np.maximum(np.linalg.norm(wn, axis=1, keepdims=True), 1e-6)
            light = ambient + np.clip(wn @ sun_dir, 0, None)[:, None] * sun
            P.append(wp); C.append(np.clip(c * light, 0, 1)); T.append(t + base)
            base += len(wp)
        if not P:
            continue
        p, c, t = np.concatenate(P), np.concatenate(C), np.concatenate(T)
        if len(t) and len(p) < 65536:
            cells[xy] = (p, c, t)
    return cells


def build_distant(db, arch, terrain, sun_dir, sun, ambient, out_path, water_z=0.0, statics=True):
    per = (VERTS - 1) // DISTANT_STEP
    h, n, c = terrain.h, terrain.n, terrain.c
    rows, cols = h.shape
    ys = np.arange(0, rows, DISTANT_STEP)
    xs = np.arange(0, cols, DISTANT_STEP)
    gy, gx = np.meshgrid(ys, xs, indexing="ij")
    height = h[gy, gx]
    # land texture colour at each sample (the tile it sits in)
    cache = {}
    tiles = terrain.tiles
    ty = np.clip(gy // 4, 0, tiles.shape[0] - 1)
    tx = np.clip(gx // 4, 0, tiles.shape[1] - 1)
    tile = tiles[ty, tx]
    base = np.zeros(height.shape + (3,))
    for v in np.unique(tile):
        base[tile == v] = texture_colour(arch, ltex_path(db, arch, v), cache)
    light = ambient + np.clip(n[gy, gx] @ sun_dir, 0, None)[..., None] * sun
    rgb = np.clip(base * c[gy, gx] * light, 0, 1)
    # the sea: flat, in the water's colour, lighter only in the shallows by the shore
    water = np.clip(ambient * 0.6 + sun * 0.45, 0, 1) * np.array([0.5, 0.62, 0.72])
    under = height < water_z
    shallow = 1.0 - np.clip((water_z - height) / 300.0, 0, 1)[..., None]
    rgb = np.where(under[..., None], water * (1.0 + 0.25 * shallow), rgb)
    height = np.where(under, water_z, height)

    # open sea around it
    m = SEA_MARGIN * per
    H = np.full((height.shape[0] + 2 * m, height.shape[1] + 2 * m), water_z)
    C = np.tile(water, (H.shape[0], H.shape[1], 1))
    H[m:m + height.shape[0], m:m + height.shape[1]] = height
    C[m:m + height.shape[0], m:m + height.shape[1]] = rgb
    x0, y0 = terrain.x0 - SEA_MARGIN, terrain.y0 - SEA_MARGIN
    nx, ny = terrain.nx + 2 * SEA_MARGIN, terrain.ny + 2 * SEA_MARGIN
    assert H.shape == (ny * per + 1, nx * per + 1), (H.shape, nx, ny, per)

    hq = np.clip(np.round(H), -32768, 32767).astype("<i2")
    cq = np.clip(np.round(C * 255), 0, 255).astype(np.uint8)
    rec = np.zeros(H.shape, dtype=[("h", "<i2"), ("r", "u1"), ("g", "u1"), ("b", "u1")])
    rec["h"], rec["r"], rec["g"], rec["b"] = hq, cq[..., 0], cq[..., 1], cq[..., 2]
    st = distant_statics(db, arch, sun_dir, sun, ambient) if statics else {}
    tris = 0
    with open(out_path, "wb") as f:
        f.write(b"MWD1" + struct.pack("<iiIII", x0, y0, nx, ny, per))
        f.write(rec.tobytes())
        f.write(b"STAT" + struct.pack("<I", len(st)))
        for (gx, gy), (p, c, t) in sorted(st.items()):
            origin = np.array([gx * 8192.0, gy * 8192.0])
            v = np.zeros(len(p), dtype=[("x", "<i2"), ("y", "<i2"), ("z", "<i2"), ("r", "u1"), ("g", "u1"),
                                        ("b", "u1"), ("pad", "u1")])
            v["x"] = np.clip(np.round((p[:, 0] - origin[0]) * 2), -32768, 32767)
            v["y"] = np.clip(np.round((p[:, 1] - origin[1]) * 2), -32768, 32767)
            v["z"] = np.clip(np.round(p[:, 2]), -32768, 32767)
            cq = np.clip(np.round(np.nan_to_num(c, nan=0.5) * 255), 0, 255).astype(np.uint8)
            v["r"], v["g"], v["b"] = cq[:, 0], cq[:, 1], cq[:, 2]
            idx = t.astype("<u2").ravel()
            f.write(struct.pack("<iiII", gx, gy, len(p), len(idx)))
            f.write(v.tobytes())
            f.write(idx.tobytes())
            if len(idx) % 2:
                f.write(b"\0\0")
            tris += len(t)
    return {"cells": nx * ny, "samples": int(H.size), "static_cells": len(st), "static_tris": tris}


def main():
    """Standalone: python tools/distant.py  (writes <MW3DS_OUT or out/data>/distant.bin for every land cell)."""
    import level
    from convert_cell import EXTERIOR_SUN_DIR
    from mwfiles import Archives, load_db
    from terrain import Terrain
    from textures import OUT
    db = load_db()
    arch = Archives()
    wx = level.weather(level.read_ini(), "Clear")
    land = [xy for xy in db["CELL"] if isinstance(xy, tuple)]
    x0, x1 = min(x for x, _ in land), max(x for x, _ in land)
    y0, y1 = min(y for _, y in land), max(y for _, y in land)
    terrain = Terrain(db, x0 - 1, y0 - 1, x1 + 1, y1 + 1)
    out = OUT / "distant.bin"
    info = build_distant(db, arch, terrain, EXTERIOR_SUN_DIR, wx["sun"], wx["ambient"], out)
    level.compress_file(out)
    print(f"distant land: {info['cells']} cells, {info['samples']} samples, {info['static_cells']} cells with "
          f"big objects ({info['static_tris']} triangles), {out.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
