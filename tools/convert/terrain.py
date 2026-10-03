"""Morrowind exterior terrain (LAND / LTEX records) -> textured, pre-lit 3DS batches.

A LAND record covers one 8192-unit exterior cell with a 65x65 vertex grid (128 units
apart) and a 16x16 grid of texture tiles (4x4 quads each). The original game blends
neighbouring tiles; here every texture used by a chunk becomes one layer:

  layer 0     the chunk's most used texture, opaque, over the whole chunk
  layer k>0   only the quads it touches, blended on top with vertex alpha
              w_k / (w_0 + ... + w_k)

where w_t is texture t's weight at the vertex (tile coverage bilinearly interpolated
between tile centres). Compositing the layers in order with those alphas gives exactly
sum(w_t * texel_t) at every vertex, i.e. a smooth blend without a blend map.
Overlay layers are coplanar with the base, so they draw with an "equal depth passes"
test and no depth writes (FLAG_DECAL).

Cells in the playable area use every vertex; the horizon ring around it uses every
second vertex plus a short skirt so its coarser edge never shows a crack.
"""
import re

import numpy as np

CELL_SIZE = 8192.0
VERTS = 65                   # per cell side
QUAD = CELL_SIZE / (VERTS - 1)
TILES = 16                   # texture tiles per cell side
TILE_SIZE = CELL_SIZE / TILES
HEIGHT_SCALE = 8.0
MISSING_HEIGHT = -512.0      # cells without LAND: sea floor, under the water plane
CHUNK_QUADS = 16             # chunk = 16 x 16 full-resolution quads (2048 units)
SKIRT = 160.0
DEFAULT_TEXTURE = "_land_default.dds"


def land_heights(rec):
    d = rec.get("VHGT") if rec is not None else None
    if d is None:
        return np.full((VERTS, VERTS), MISSING_HEIGHT)
    offset = np.frombuffer(d, "<f4", 1)[0]
    delta = np.frombuffer(d, np.int8, VERTS * VERTS, 4).reshape(VERTS, VERTS).astype(np.float64)
    # Column 0 accumulates down the rows; every row then accumulates along x
    row_start = offset + np.cumsum(delta[:, 0])
    along = np.concatenate([np.zeros((VERTS, 1)), np.cumsum(delta[:, 1:], axis=1)], axis=1)
    return (row_start[:, None] + along) * HEIGHT_SCALE


def land_normals(rec):
    d = rec.get("VNML") if rec is not None else None
    if d is None:
        n = np.zeros((VERTS, VERTS, 3))
        n[..., 2] = 1.0
        return n
    n = np.frombuffer(d, np.int8, VERTS * VERTS * 3).reshape(VERTS, VERTS, 3).astype(np.float64)
    n[np.all(n == 0, axis=2)] = (0, 0, 1)
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def land_colors(rec):
    d = rec.get("VCLR") if rec is not None else None
    if d is None:
        return np.ones((VERTS, VERTS, 3))
    return np.frombuffer(d, np.uint8, VERTS * VERTS * 3).reshape(VERTS, VERTS, 3) / 255.0


def land_textures(rec):
    """16x16 [row y][column x] of VTEX values (0 = default texture, else LTEX index + 1).
    The record stores them as a 4x4 grid of 4x4 blocks."""
    d = rec.get("VTEX") if rec is not None else None
    out = np.zeros((TILES, TILES), dtype=np.int32)
    if d is None:
        return out
    raw = np.frombuffer(d, "<u2", TILES * TILES)
    i = 0
    for y1 in range(4):
        for x1 in range(4):
            for y2 in range(4):
                for x2 in range(4):
                    out[y1 * 4 + y2, x1 * 4 + x2] = raw[i]
                    i += 1
    return out


class Terrain:
    """Heights, normals, colours and texture tiles of a rectangle of exterior cells,
    stitched into global grids so chunks can read across cell borders."""

    def __init__(self, db, x0, y0, x1, y1):
        self.x0, self.y0, self.nx, self.ny = x0, y0, x1 - x0 + 1, y1 - y0 + 1
        nvx, nvy = self.nx * (VERTS - 1) + 1, self.ny * (VERTS - 1) + 1
        self.h = np.full((nvy, nvx), MISSING_HEIGHT)
        self.n = np.zeros((nvy, nvx, 3))
        self.n[..., 2] = 1.0
        self.c = np.ones((nvy, nvx, 3))
        self.tiles = np.zeros((self.ny * TILES, self.nx * TILES), dtype=np.int32)
        self.has_land = np.zeros((self.ny, self.nx), dtype=bool)
        for cy in range(self.ny):
            for cx in range(self.nx):
                rec = db["LAND"].get((x0 + cx, y0 + cy))
                vy, vx = cy * (VERTS - 1), cx * (VERTS - 1)
                self.h[vy:vy + VERTS, vx:vx + VERTS] = land_heights(rec)
                self.n[vy:vy + VERTS, vx:vx + VERTS] = land_normals(rec)
                self.c[vy:vy + VERTS, vx:vx + VERTS] = land_colors(rec)
                self.tiles[cy * TILES:(cy + 1) * TILES, cx * TILES:(cx + 1) * TILES] = land_textures(rec)
                self.has_land[cy, cx] = rec is not None and rec.get("VHGT") is not None
        self.origin = np.array([x0 * CELL_SIZE, y0 * CELL_SIZE])

    def texture_weights(self, vx, vy, tex_ids):
        """{texture value: weights} at global vertex coordinates (arrays of equal shape)."""
        nty, ntx = self.tiles.shape
        u, v = vx / 4.0 - 0.5, vy / 4.0 - 0.5          # tile centres sit at 4k + 2
        u0, v0 = np.floor(u).astype(int), np.floor(v).astype(int)
        fu, fv = u - u0, v - v0
        out = {t: np.zeros(vx.shape) for t in tex_ids}
        for dy, wy in ((0, 1 - fv), (1, fv)):
            for dx, wx in ((0, 1 - fu), (1, fu)):
                tile = self.tiles[np.clip(v0 + dy, 0, nty - 1), np.clip(u0 + dx, 0, ntx - 1)]
                for t in tex_ids:
                    out[t] += np.where(tile == t, wx * wy, 0.0)
        return out

    def chunk(self, cx, cy, step, sun_dir, sun, ambient, skirt=False):
        """One chunk (chunk grid coordinates within this terrain) as layers:
        [(texture value, positions Nx3, uv Nx2, rgba Nx4 floats, triangles Mx3)], base layer first."""
        q = CHUNK_QUADS
        xs = np.arange(cx * q, cx * q + q + 1, step)
        ys = np.arange(cy * q, cy * q + q + 1, step)
        gx, gy = np.meshgrid(xs, ys)                     # [row y][column x]
        n = len(xs)
        pos = np.stack([self.origin[0] + gx * QUAD, self.origin[1] + gy * QUAD, self.h[gy, gx]], axis=-1)
        nrm = self.n[gy, gx]
        light = ambient + np.clip(nrm @ sun_dir, 0, None)[..., None] * sun
        rgb = np.clip(self.c[gy, gx] * light, 0, 1)
        # Chunk-local UVs keep texture coordinates small (one repeat per texture tile)
        uv = np.stack([(gx - cx * q) * QUAD / TILE_SIZE, (gy - cy * q) * QUAD / TILE_SIZE], axis=-1)

        # Textures touching this chunk (tiles one past its edges bleed in)
        t0y, t0x = max(cy * q // 4 - 1, 0), max(cx * q // 4 - 1, 0)
        used, counts = np.unique(self.tiles[t0y:(cy + 1) * q // 4 + 1, t0x:(cx + 1) * q // 4 + 1], return_counts=True)
        weights = self.texture_weights(gx.astype(float), gy.astype(float), list(used))
        order = sorted(used, key=lambda t: -weights[t].sum())

        # Two triangles per quad
        i = np.arange(n - 1)
        a = (i[:, None] * n + i[None, :]).ravel()        # lower-left corner of each quad
        quads = np.stack([a, a + 1, a + n + 1, a + n], axis=1)
        tris_all = np.concatenate([quads[:, [0, 1, 2]], quads[:, [0, 2, 3]]])

        flat_pos, flat_uv, flat_rgb = pos.reshape(-1, 3), uv.reshape(-1, 2), rgb.reshape(-1, 3)
        layers, acc = [], np.zeros(gx.shape)
        for k, t in enumerate(order):
            w = weights[t]
            acc = acc + w
            if k == 0:
                alpha = np.ones(gx.shape)
                tris = tris_all
            else:
                alpha = np.where(acc > 1e-6, w / np.maximum(acc, 1e-6), 0.0)
                touched = (w.ravel()[quads] > 1e-3).any(axis=1)
                if not touched.any():
                    continue
                tris = np.concatenate([quads[touched][:, [0, 1, 2]], quads[touched][:, [0, 2, 3]]])
            rgba = np.concatenate([flat_rgb, alpha.reshape(-1, 1)], axis=1)
            p, u, c, tr = flat_pos, flat_uv, rgba, tris
            if k == 0 and skirt:
                p, u, c, tr = add_skirt(p, u, c, tr, n)
            elif k > 0:
                # Keep only the vertices this overlay's quads use
                keep, tr = np.unique(tr, return_inverse=True)
                p, u, c, tr = p[keep], u[keep], c[keep], tr.reshape(-1, 3)
            layers.append((int(t), p, u, c, tr))
        return layers

    def min_height(self, cx, cy):
        q = CHUNK_QUADS
        return float(self.h[cy * q:cy * q + q + 1, cx * q:cx * q + q + 1].min())

    def collision(self, x0, y0, x1, y1):
        """Full-resolution triangles over the cell rectangle [x0..x1] x [y0..y1] (cell grid coords)."""
        vx0, vy0 = (x0 - self.x0) * (VERTS - 1), (y0 - self.y0) * (VERTS - 1)
        vx1, vy1 = (x1 - self.x0 + 1) * (VERTS - 1), (y1 - self.y0 + 1) * (VERTS - 1)
        xs, ys = np.arange(vx0, vx1 + 1), np.arange(vy0, vy1 + 1)
        gx, gy = np.meshgrid(xs, ys)
        n = len(xs)
        pos = np.stack([self.origin[0] + gx * QUAD, self.origin[1] + gy * QUAD, self.h[gy, gx]], axis=-1).reshape(-1, 3)
        ix, iy = np.meshgrid(np.arange(n - 1), np.arange(len(ys) - 1))
        a = (iy * n + ix).ravel()
        tris = np.concatenate([np.stack([a, a + 1, a + n + 1], 1), np.stack([a, a + n + 1, a + n], 1)])
        return pos, tris


def add_skirt(pos, uv, rgba, tris, n):
    """Hangs a vertical strip under the chunk's border so coarser neighbours leave no gaps."""
    idx = np.arange(n * n).reshape(n, n)
    border = np.concatenate([idx[0, :], idx[1:, -1], idx[-1, -2::-1], idx[-2:0:-1, 0]])
    ring = np.append(border, border[0])
    base = len(pos)
    low = pos[ring] - [0.0, 0.0, SKIRT]
    top, bottom = ring, base + np.arange(len(ring))
    j = np.arange(len(ring) - 1)
    skirt = np.concatenate([np.stack([top[j], top[j + 1], bottom[j + 1]], 1),
                            np.stack([top[j], bottom[j + 1], bottom[j]], 1)])
    return (np.concatenate([pos, low]), np.concatenate([uv, uv[ring]]),
            np.concatenate([rgba, rgba[ring]]), np.concatenate([tris, skirt]))


def ltex_path(db, arch, value):
    """Archive path of a VTEX value's texture; LTEX names say .tga but the game ships .dds."""
    name = DEFAULT_TEXTURE if value == 0 else db["LTEX"].get(value - 1) or DEFAULT_TEXTURE
    name = name.lower().replace("/", "\\")
    name = name if name.startswith("textures\\") else "textures\\" + name
    dds = re.sub(r"\.[^.\\]+$", ".dds", name)
    return dds if arch.exists(dds) or not arch.exists(name) else name
