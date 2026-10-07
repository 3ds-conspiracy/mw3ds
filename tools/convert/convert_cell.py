"""Converts a Morrowind interior cell, or a block of exterior cells, into a baked 3DS cell package.

All static geometry is pre-transformed to world space, lit on the CPU (Morrowind's
lighting is per-vertex anyway), and merged into batches that share a texture and
render state within one spatial chunk, so the 3DS draws a few big buffers and can
skip whole chunks that are behind the camera or past the fog.

Exteriors add terrain (terrain.py), a water plane and a sky dome.

Output under out/data/:
  cells/<cell>.cel       see write_cell() for the layout
  cells/<cell>.act       animated NPCs (actors.py)
  textures/<name>.t3x    encoded later by textures.TextureSet
"""
import hashlib
import json
import math
import re
import struct
from pathlib import Path

import numpy as np

from mwfiles import cell_refs
from meshes import (FLAG_BLEND, FLAG_DECAL, FLAG_SCROLL, FLAG_TEST, FLAG_TWO_SIDED,
                    collision_triangles, extract_shapes, find_node, ref_matrix)
from nif import Nif
from npc import NpcBuilder
from creature import CreatureBuilder, resolve_leveled
from actors import (actor_mesh, merge_meshes, skeleton_export, write_actor_library, write_actor_placements,
                    write_actors)
from textures import shard_dir, texture_out_name
from terrain import CELL_SIZE, CHUNK_QUADS, QUAD, TILE_SIZE, ltex_path
from textures import OUT, texels_needed

# Morrowind.ini light attenuation defaults: linear only, 3 / radius
LINEAR_ATTEN = 3.0
INTERIOR_SUN_DIR = np.array([-0.15, 0.15, 1.0]) / np.linalg.norm([-0.15, 0.15, 1.0])
# Mid-morning sun, from the east-south-east
EXTERIOR_SUN_DIR = np.array([0.7, -0.25, 0.65]) / np.linalg.norm([0.7, -0.25, 0.65])
LIGHT_NEGATIVE, LIGHT_OFF_DEFAULT = 0x4, 0x20

VERTEX = np.dtype([("pos", "<f4", 3), ("uv", "<f4", 2), ("color", "u1", 4)])

# Objects that block actors; carried items (MISC, BOOK, ...) do not
COLLIDING_TYPES = {"STAT", "DOOR", "ACTI", "CONT", "LIGH"}
# Not drawn: leveled item lists have no mesh (leveled creatures resolve to a creature)
SKIPPED_TYPES = {"LEVI"}
LIGHT_CAN_CARRY = 0x2
GRID_CELL = 128.0
ACTOR_RADIUS, ACTOR_HEIGHT = 28.0, 128.0

INTERIOR_CHUNK = 4096.0
EXTERIOR_CHUNK = CHUNK_QUADS * QUAD        # 2048, the terrain chunk size
WATER_TILE = 1024.0                        # world units per water texture repeat
SKY_RADIUS = 3000.0
FOG_START, FOG_END = 1800.0, 6000.0        # exterior view distance; the horizon ring is one cell wide
# Exterior clutter (references smaller than DETAIL_SIZE across) is batched separately and not drawn
# past DETAIL_DISTANCE: it is most of the draw calls in a town and nearly invisible in the fog
DETAIL_SIZE, DETAIL_DISTANCE = 160.0, 2500.0


def build_grid(pos, tris):
    """2D uniform grid over XY: cell -> triangle indices overlapping it (ascending)."""
    lo = pos[:, :2].min(axis=0) - 1.0
    hi = pos[:, :2].max(axis=0) + 1.0
    nx, ny = (np.ceil((hi - lo) / GRID_CELL).astype(int) + 1).tolist()
    tp = pos[tris]
    tlo = ((tp[:, :, :2].min(axis=1) - lo) // GRID_CELL).astype(np.int64)
    thi = ((tp[:, :, :2].max(axis=1) - lo) // GRID_CELL).astype(np.int64)
    span = thi - tlo + 1
    counts = span[:, 0] * span[:, 1]
    tri = np.repeat(np.arange(len(tris), dtype=np.int64), counts)
    off = np.arange(counts.sum()) - np.repeat(np.cumsum(counts) - counts, counts)
    gx = tlo[tri, 0] + off % span[tri, 0]
    gy = tlo[tri, 1] + off // span[tri, 0]
    cell = gy * nx + gx
    order = np.lexsort((tri, cell))
    start = np.searchsorted(cell[order], np.arange(nx * ny + 1)).astype(np.uint32)
    return lo, nx, ny, start, tri[order].astype(np.uint32)


# ---- lighting ----

def bake_colors(s, lights, ambient, sun, sun_dir=INTERIOR_SUN_DIR):
    if "rgba" in s:                        # terrain, water, sky: colours computed by their builders
        return (np.clip(s["rgba"], 0, 1) * 255 + 0.5).astype(np.uint8)
    n = len(s["pos"])
    mat = s["mat"]
    amb = np.array(mat["ambient"]) if mat else np.ones(3)
    dif = np.array(mat["diffuse"]) if mat else np.ones(3)
    emi = np.array(mat["emissive"]) if mat else np.zeros(3)
    alpha = np.full(n, mat["alpha"] if mat else 1.0)
    amb, dif, emi = (np.tile(c, (n, 1)) for c in (amb, dif, emi))
    vertex_mode = s["vcp"]["vertex_mode"] if s["vcp"] else 2
    lighting_mode = s["vcp"]["lighting_mode"] if s["vcp"] else 1
    if s["vcol"] is not None:
        if vertex_mode == 2:
            amb = dif = s["vcol"][:, :3]
            alpha = alpha * s["vcol"][:, 3]
        elif vertex_mode == 1:
            emi = s["vcol"][:, :3]

    if lighting_mode == 0:
        color = emi
    else:
        pos, nrm = s["pos"], s["nrm"]
        light = np.clip(nrm @ sun_dir, 0, None)[:, None] * sun
        lo, hi = pos.min(axis=0), pos.max(axis=0)
        for L in lights:
            reach = 2 * L["radius"]
            if np.any(L["pos"] < lo - reach) or np.any(L["pos"] > hi + reach):
                continue
            d = L["pos"] - pos
            dist = np.maximum(np.linalg.norm(d, axis=1), 1.0)
            ndl = np.clip(np.sum(nrm * d, axis=1) / dist, 0, None)
            att = np.minimum(1.0, L["radius"] / (LINEAR_ATTEN * dist))
            fade = 1.0 - np.clip(dist / L["radius"] - 1.0, 0, 1) ** 2
            light += (ndl * att * fade)[:, None] * L["color"]
        color = emi + amb * ambient + dif * light
    rgba = np.concatenate([np.clip(color, 0, 1), np.clip(alpha, 0, 1)[:, None]], axis=1)
    return (np.nan_to_num(rgba, nan=1.0) * 255 + 0.5).astype(np.uint8)


# Actor libraries are lit evenly at this level; each placement's tint (x 64 in a byte) scales it to the
# light where it stands: ambient, the sun, and the point lights' reach there
NEUTRAL_LIGHT = 0.5


def light_tint(pos, lights, ambient, sun):
    light = np.asarray(ambient, dtype=float) + 0.5 * np.asarray(sun, dtype=float)
    for L in lights:
        d = float(np.linalg.norm(L["pos"] - pos))
        dist = max(d, 1.0)
        att = min(1.0, L["radius"] / (LINEAR_ATTEN * dist))
        fade = 1.0 - min(max(dist / L["radius"] - 1.0, 0.0), 1.0) ** 2
        light = light + att * fade * np.asarray(L["color"])
    return [int(v) for v in np.clip(np.round(light / NEUTRAL_LIGHT * 64), 0, 255)]


# ---- cell file ----

def planar(records):
    """(n, size) bytes -> bytes as planes: byte 0 of every record, then byte 1 ... (planes.h undoes it)"""
    return np.ascontiguousarray(np.asarray(records, dtype=np.uint8).T).tobytes()


def deltas(a, bits):
    """Integers (along axis 0) as the difference from the previous one, wrapping at `bits`."""
    a = np.asarray(a).astype(np.int64)
    d = np.diff(a, axis=0, prepend=np.zeros((1,) + a.shape[1:], dtype=np.int64))
    return (d & ((1 << bits) - 1)).astype(np.uint16 if bits == 16 else np.uint32)


def packed_ints(a, width):
    """Indices etc. as planes of differences (cell format 7)."""
    d = deltas(np.asarray(a).ravel(), width * 8).astype("<u2" if width == 2 else "<u4")
    return planar(d.view(np.uint8).reshape(-1, width))


def write_batches(f, batches):
    """Batches of one group (a cell's static batches, one door, or the sky) share one position grid:
    s16 = (pos - offset) / scale, so coplanar terrain layers in different batches quantize identically.
    UVs are s16 scaled per batch. 16 bytes per vertex instead of 24."""
    f.write(struct.pack("<I", len(batches)))
    if not batches:
        return
    allpos = np.concatenate([b["verts"]["pos"] for b in batches])
    lo, hi = allpos.min(axis=0), allpos.max(axis=0)
    scale = np.maximum(hi - lo, 1e-3) / 65534.0
    offset = lo + 32767.0 * scale
    for b in batches:
        v = b["verts"]
        p = v["pos"]
        uv_scale = np.maximum(np.abs(v["uv"]).max(axis=0) if len(v) else np.ones(2), 1e-4) / 32767.0
        f.write(struct.pack("<iBBHII6f8f", b["tex"], b["flags"], b["alpha_ref"], int(b.get("max_dist", 0) // 8),
                            len(v), len(b["idx"]), *p.min(axis=0), *p.max(axis=0),
                            *offset, *scale, *uv_scale))
        q = np.zeros(len(v), dtype=[("pos", "<i2", 4), ("uv", "<i2", 2), ("color", "u1", 4)])
        q["pos"][:, :3] = np.clip(np.round((p - offset) / scale), -32767, 32767)
        q["uv"] = np.clip(np.round(v["uv"] / uv_scale), -32767, 32767)
        q["color"] = v["color"]
        # Format 7: positions as differences from the previous vertex, then everything as byte planes
        rec = q.view(np.uint8).reshape(-1, 16).copy()
        if len(q):
            rec[:, 0:6] = deltas(q["pos"][:, :3].view(np.uint16), 16).astype("<u2").view(np.uint8).reshape(-1, 6)
        f.write(planar(rec))
        f.write(packed_ints(b["idx"], 2))
        if len(b["idx"]) & 1:
            f.write(b"\0\0")


CELL_INTERIOR, CELL_WATER, CELL_SKY = 1, 2, 4


def write_collision(f, pos, tris, ranges=None):
    """Welded, quantized collision: vertices snap to an s16 grid over the cell (under 0.2 units apart),
    duplicates merge, triangles that collapse are dropped; indices shrink to u16 when they fit.
    ranges {key: (first triangle, count)} in the input come back as ranges in the written triangles."""
    pos = np.asarray(pos, dtype=np.float64)
    tris = np.asarray(tris, dtype=np.int64).reshape(-1, 3)
    lo, hi = pos.min(axis=0), pos.max(axis=0)
    scale = np.maximum(hi - lo, 1e-3) / 65534.0
    offset = lo + 32767.0 * scale
    q = np.clip(np.round((pos - offset) / scale), -32767, 32767).astype(np.int16)
    uq, remap = np.unique(q, axis=0, return_inverse=True)
    t = remap.reshape(-1)[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t = t[keep]
    kept = np.concatenate([[0], np.cumsum(keep)])
    out_ranges = {k: (int(kept[a]), int(kept[a + n] - kept[a])) for k, (a, n) in (ranges or {}).items()}
    if not len(t):
        uq, t = np.zeros((3, 3), dtype=np.int16), np.array([[0, 1, 2]])
    welded = offset + uq.astype(np.float64) * scale
    glo, nx, ny, start, flat = build_grid(welded, t)
    iw = 2 if len(uq) < 65536 else 4
    gw = 2 if len(t) < 65536 else 4

    def indices(a, width):
        a = np.asarray(a).ravel()
        return packed_ints(a, width) + (b"\0\0" if width == 2 and len(a) & 1 else b"")

    # Format 7: vertices, indices and grid lists as planes of differences
    f.write(struct.pack("<I6f", len(uq), *offset, *scale))
    vq = deltas(uq.astype(np.int64).view(np.int64) & 0xFFFF, 16).astype("<u2").view(np.uint8).reshape(-1, 6)
    f.write(planar(vq) + (b"\0\0" if uq.size & 1 else b""))
    f.write(struct.pack("<II", len(t), iw | (gw << 8)))
    f.write(indices(t, iw))
    f.write(struct.pack("<3f2I", glo[0], glo[1], GRID_CELL, nx, ny))
    f.write(packed_ints(start, 4) + indices(flat, gw))
    return out_ranges


def write_cell(path, info, textures, batches, collision, actors, door_meshes, sky, col_ranges=None):
    """Layout (little endian):
       'MWC1' u32 version=6 u32 flags (bit0 interior, bit1 water plane, bit2 sky)
       f32 spawn[3] f32 yaw f32 pitch
       u8 ambient[4] u8 fog[4] f32 fog_density
       f32 fog_start f32 fog_end (0 = no fog) f32 water_z
       f32 bounds[4] min x, min y, max x, max y of the walkable area (all 0 = unbounded)
       u32 num_textures, then 64-byte zero-padded names (relative to data/textures)
       u32 num_batches, then per batch:
         i32 texture (-1 none) u8 flags u8 alpha_ref u16 max_distance / 8 (0 = any) u32 num_vertices u32 num_indices
         f32 bbox_min[3] f32 bbox_max[3] f32 pos_offset[3] f32 pos_scale[3] f32 uv_scale[2]
         vertices (16 bytes: s16 pos[4] (w unused), s16 uv[2], u8 rgba[4]; pos = offset + s16 * scale,
         uv = s16 * uv_scale), u16 indices, pad to 4 bytes
         flags: 1 blend, 2 alpha test, 4 two-sided, 8 clamp, 16 additive,
                32 decal (coplanar overlay: equal depth passes, no depth write), 64 scrolling UVs,
                128 rising UVs (smoke, steam)
       collision (see write_collision): u32 num_vertices, f32 offset[3], f32 scale[3], s16 pos[3]... (pad 4),
         u32 num_triangles, u32 widths (byte 0 triangle index bytes, byte 1 grid list bytes: 2 or 4),
         idx[3]... (pad 4), f32 grid_origin[2], f32 grid_cell, u32 nx, u32 ny, u32 cell_start[nx*ny+1],
         cell_tris[] (pad 4)
       actors: u32 count, then per actor: char id[32], char name[32], f32 pos[3], f32 yaw,
         f32 radius, f32 height
       swing doors: u32 count, then per door: u32 ref index, batches as above in object space
       sky: batches as above around the origin, drawn centred on the camera without fog
    """
    flags = (CELL_INTERIOR if info["interior"] else 0) | (CELL_WATER if info["water"] else 0) \
        | (CELL_SKY if sky else 0)
    with open(path, "wb") as f:
        f.write(b"MWC1" + struct.pack("<II", 7, flags))
        f.write(struct.pack("<5f", *info["spawn"], info["yaw"], 0.0))
        f.write(bytes(info["ambient"]) + bytes(info["fog"]) + struct.pack("<f", info["fog_density"]))
        f.write(struct.pack("<3f", info["fog_start"], info["fog_end"], info["water_z"]))
        f.write(struct.pack("<4f", *info["bounds"]))
        f.write(struct.pack("<I", len(textures)))
        for t in textures:
            f.write(t.encode("ascii").ljust(64, b"\0"))
        write_batches(f, batches)
        ranges = write_collision(f, *collision, ranges=col_ranges)
        f.write(struct.pack("<I", len(actors)))
        for a in actors:
            f.write(a["id"].encode("latin-1")[:31].ljust(32, b"\0"))
            f.write(a["name"].encode("latin-1")[:31].ljust(32, b"\0"))
            f.write(struct.pack("<6f", *a["pos"], a["yaw"], ACTOR_RADIUS, ACTOR_HEIGHT))
        f.write(struct.pack("<I", len(door_meshes)))
        for d in door_meshes:
            f.write(struct.pack("<I", d["ref"]))
            write_batches(f, d["batches"])
        write_batches(f, sky)
    return ranges


def color3(d, off):
    return np.array(list(d[off:off + 3]), dtype=float) / 255.0


def is_marker(model):
    return model.lower().replace("/", "\\").rsplit("\\", 1)[-1].startswith("marker")


def world_shapes(shapes, m):
    for s in shapes:
        s["pos"] = s["pos"] @ m[:3, :3].T + m[:3, 3]
        s["nrm"] = s["nrm"] @ m[:3, :3].T
        s["nrm"] /= np.maximum(np.linalg.norm(s["nrm"], axis=1, keepdims=True), 1e-6)
    return shapes


def make_batches(groups, tex_index, lights, ambient, sun, sun_dir, ranges=None):
    """Merges shape groups into batches (opaque, then alpha-tested, then blended; stable
    otherwise, so terrain layers keep their order). Group keys start with the render key
    (tex, flags, alpha_ref). With `ranges`, records where each shape's triangles land:
    ranges[ref] += [batch, first, count]."""
    batches = []
    order = lambda k: (bool(k[1] & FLAG_BLEND), bool(k[1] & FLAG_TEST))
    for key in sorted(groups, key=order):
        tex, flags, alpha_ref = key[:3]
        max_dist = key[4] if len(key) > 4 else 0
        cur_v, cur_i, base, first = [], [], 0, 0
        for s in groups[key]:
            n = len(s["pos"])
            if base + n > 65535 and cur_v:
                batches.append({"verts": np.concatenate(cur_v), "idx": np.concatenate(cur_i),
                                "tex": tex_index.get(tex, -1), "flags": flags, "alpha_ref": alpha_ref,
                                "max_dist": max_dist})
                cur_v, cur_i, base, first = [], [], 0, 0
            v = np.zeros(n, dtype=VERTEX)
            v["pos"] = s["local_pos"] if "local_pos" in s else s["pos"]
            v["uv"] = s["uv"] * [1.0, -1.0] + [0.0, 1.0]   # DirectX V (0 = top) -> PICA V (0 = bottom)
            v["color"] = bake_colors(s, lights, ambient, sun, sun_dir)
            cur_v.append(v)
            cur_i.append(np.asarray(s["tris"]).ravel() + base)
            if ranges is not None and s.get("ref") is not None:
                ranges.setdefault(s["ref"], []).append([len(batches), first, len(np.asarray(s["tris"]).ravel())])
            first += len(np.asarray(s["tris"]).ravel())
            base += n
        if cur_v:
            batches.append({"verts": np.concatenate(cur_v), "idx": np.concatenate(cur_i),
                            "tex": tex_index.get(tex, -1), "flags": flags, "alpha_ref": alpha_ref,
                            "max_dist": max_dist})
    return batches


SMALL_BATCH_TRIS = 400       # a texture with fewer triangles than this in the cell is one batch, not per chunk


def merge_small_groups(groups):
    """A texture used only by a few small objects scattered over several chunks would become a
    handful of tiny batches, each a draw call; below SMALL_BATCH_TRIS triangles in the whole cell
    they share one batch (its bigger bounds cost less than the extra draws). Terrain and water
    groups keep their chunks (terrain layers must stay per chunk, in order)."""
    totals = {}
    for key, shapes in groups.items():
        if isinstance(key[3], tuple) and len(key[3]) == 2 and not isinstance(key[3][0], str):
            k = key[:3] + key[4:]
            totals[k] = totals.get(k, 0) + sum(len(np.asarray(s["tris"]).ravel()) // 3 for s in shapes)
    out = {}
    for key, shapes in groups.items():
        k = key[:3] + key[4:]
        if k in totals and totals[k] < SMALL_BATCH_TRIS:
            key = key[:3] + ("cell",) + key[4:]
        out.setdefault(key, []).extend(shapes)
    return out


def cell_file_name(cell_name):
    return re.sub(r"[^a-z0-9_]", "_", cell_name.lower())


# ---- exterior extras ----

def terrain_shapes(db, arch, terrain, gx, gy, full, sun, ambient, textures):
    """Terrain chunk layers of exterior cell (gx, gy) as pre-coloured shapes, keyed by chunk.
    full: every vertex (walkable cells); else every second one plus a skirt (horizon cells)."""
    shapes = []
    per_cell = int(CELL_SIZE // EXTERIOR_CHUNK)
    tx, ty = gx - terrain.x0, gy - terrain.y0
    if not terrain.has_land[ty, tx]:
        return shapes
    for cy in range(ty * per_cell, (ty + 1) * per_cell):
        for cx in range(tx * per_cell, (tx + 1) * per_cell):
            layers = terrain.chunk(cx, cy, 1 if full else 2, EXTERIOR_SUN_DIR, sun, ambient, skirt=not full)
            for k, (value, pos, uv, rgba, tris) in enumerate(layers):
                path = ltex_path(db, arch, value)
                textures.request(path, texels_needed(TILE_SIZE), False)
                flags = FLAG_TWO_SIDED | (FLAG_BLEND | FLAG_DECAL if k else 0)
                shapes.append({"pos": pos, "uv": uv, "rgba": rgba, "tris": tris, "tex": path,
                               "flags": flags, "alpha_ref": 0, "chunk": ("terrain", cx, cy)})
    return shapes


def find_water_texture(arch):
    names = sorted(arch.names())
    for pat in (r"textures\\water\\water0*0\.dds$", r"textures\\water0*0\.dds$", r"textures\\.*water.*\.dds$"):
        hit = [n for n in names if re.search(pat, n)]
        if hit:
            return hit[0]
    return None


def water_shapes(arch, terrain, gx, gy, water_z, sun, ambient, textures):
    """One quad per terrain chunk of cell (gx, gy) that dips below the water, batched per cell."""
    tex = find_water_texture(arch)
    if tex:
        textures.request(tex, 128, False)
    color = np.append(np.clip(ambient * 0.6 + sun * 0.45, 0, 1), 0.7)
    shapes = []
    per_cell = int(CELL_SIZE // EXTERIOR_CHUNK)
    tx, ty = gx - terrain.x0, gy - terrain.y0
    for cy in range(ty * per_cell, (ty + 1) * per_cell):
        for cx in range(tx * per_cell, (tx + 1) * per_cell):
            if terrain.has_land[ty, tx] and terrain.min_height(cx, cy) >= water_z:
                continue
            x0 = terrain.origin[0] + cx * EXTERIOR_CHUNK
            y0 = terrain.origin[1] + cy * EXTERIOR_CHUNK
            pos = np.array([[x0, y0, water_z], [x0 + EXTERIOR_CHUNK, y0, water_z],
                            [x0 + EXTERIOR_CHUNK, y0 + EXTERIOR_CHUNK, water_z], [x0, y0 + EXTERIOR_CHUNK, water_z]])
            uv = (pos[:, :2] - [x0, y0]) / WATER_TILE
            shapes.append({"pos": pos, "uv": uv, "rgba": np.tile(color, (4, 1)), "tris": np.array([0, 1, 2, 0, 2, 3]),
                           "tex": tex, "flags": FLAG_BLEND | FLAG_SCROLL | FLAG_TWO_SIDED, "alpha_ref": 0,
                           "chunk": ("water", gx, gy)})
    return shapes


def interior_water_shapes(arch, groups, water_z, sun, ambient, textures, chunk_of):
    """An interior's water: a quad for every square of the floor plan where some triangle dips below
    the water level, as the exteriors' water quads (same texture, colour and scrolling)."""
    tile = WATER_TILE
    tiles = set()
    for shapes in groups.values():
        for s in shapes:
            pos, tris = s["pos"], np.asarray(s["tris"]).reshape(-1, 3)
            if not len(tris):
                continue
            tp = pos[tris]
            low = tp.min(axis=1)[:, 2] < water_z
            for lo, hi in zip(tp.min(axis=1)[low], tp.max(axis=1)[low]):
                for tx in range(int(math.floor(lo[0] / tile)), int(math.floor(hi[0] / tile)) + 1):
                    for ty in range(int(math.floor(lo[1] / tile)), int(math.floor(hi[1] / tile)) + 1):
                        tiles.add((tx, ty))
    tex = find_water_texture(arch)
    if tex and tiles:
        textures.request(tex, 128, False)
    color = np.append(np.clip(ambient * 0.6 + sun * 0.45, 0, 1), 0.7)
    shapes = []
    for tx, ty in sorted(tiles):
        x0, y0 = tx * tile, ty * tile
        pos = np.array([[x0, y0, water_z], [x0 + tile, y0, water_z], [x0 + tile, y0 + tile, water_z],
                        [x0, y0 + tile, water_z]])
        uv = (pos[:, :2] - [x0, y0]) / WATER_TILE
        s = {"pos": pos, "uv": uv, "rgba": np.tile(color, (4, 1)), "tris": np.array([0, 1, 2, 0, 2, 3]),
             "tex": tex, "flags": FLAG_BLEND | FLAG_SCROLL | FLAG_TWO_SIDED, "alpha_ref": 0}
        s["chunk"] = chunk_of(s)
        shapes.append(s)
    return shapes


def sky_batches(arch, weather, textures, tex_index, tex_names):
    """Sky dome around the origin: a colour gradient (fog colour at and below the horizon,
    sky colour overhead) and a scrolling cloud layer that fades out towards the horizon."""
    seg = 16
    rings = [-30, -5, 0, 8, 20, 40, 65, 90]
    fog, sky = weather["fog"], weather["sky"]

    def dome(elevations, color_of, alpha_of):
        pos, uv, rgba = [], [], []
        for e in elevations:
            er = math.radians(e)
            for i in range(seg + 1):
                a = 2 * math.pi * i / seg
                p = np.array([math.cos(er) * math.sin(a), math.cos(er) * math.cos(a), math.sin(er)]) * SKY_RADIUS
                pos.append(p)
                uv.append(p[:2] / SKY_RADIUS * 1.5)
                rgba.append(np.append(color_of(e), alpha_of(e)))
        tris = []
        for r in range(len(elevations) - 1):
            for i in range(seg):
                a, b = r * (seg + 1) + i, (r + 1) * (seg + 1) + i
                tris += [a, a + 1, b + 1, a, b + 1, b]
        return np.array(pos), np.array(uv), np.array(rgba), np.array(tris)

    def gradient(e):
        t = np.clip(e / 40.0, 0, 1) ** 0.7
        return fog * (1 - t) + sky * t

    shapes = []
    pos, uv, rgba, tris = dome(rings, gradient, lambda e: 1.0)
    shapes.append(({"pos": pos, "uv": uv, "rgba": rgba, "tris": tris}, None, FLAG_TWO_SIDED))
    cloud = "textures\\" + re.sub(r"\.[^.]+$", ".dds", weather["clouds"].lower())
    if arch.exists(cloud):
        cloud_rings = [3, 10, 20, 40, 65, 90]
        pos, uv, rgba, tris = dome(cloud_rings, lambda e: np.ones(3),
                                   lambda e: float(np.clip((e - 3) / 25.0, 0, 1)) * 0.9)
        textures.request(cloud, 256, True)
        shapes.append(({"pos": pos, "uv": uv, "rgba": rgba, "tris": tris}, cloud,
                       FLAG_BLEND | FLAG_SCROLL | FLAG_TWO_SIDED))
    out = []
    for s, tex, flags in shapes:
        if tex and tex not in tex_index:
            tex_index[tex] = len(tex_names)
            tex_names.append(textures.request(tex, 0, False))
        out += make_batches({(tex, flags, 0): [s]}, tex_index, [], None, None, None)
    return out


# ---- conversion ----

MOVE_FUNCS = r"(?:rotate|rotateworld|move|moveworld|setpos|setangle|setatstart)"
DECORATIVE_MOVERS = {"float", "signrotate"}     # bobbing crates, swinging signs: left baked (draw calls)


TOGGLE = re.compile(r'(?im)"?([^"\n;]+?)"?\s*->\s*(?:enable|disable)\b')


def toggled_objects(db):
    """Ids of objects scripts or dialogue enable / disable: their collision is kept apart (by triangle
    range) so the game can switch it with them (Tarhiel's platform, Nchuleftingth's wall, the puzzle
    canal's force field and bridges, the Boethiah shrine's scaffolds)."""
    texts = list(db["SCPT"].values()) + [i.zstr("BNAM") or "" for d in db["dialogue"] for i in d["infos"]]
    names = {m.group(1).strip().lower() for t in texts for m in TOGGLE.finditer(t)}
    return {n for n in names if n in db["objects"] and db["objects"][n].tag not in ("NPC_", "CREA")}


def moving_objects(db):
    """Ids of objects (not actors) scripts move or turn: their own script's Rotate / Move / SetPos ...,
    or another script's "id"->Rotate ... (Ghostgate's portcullis, Dagoth Ur's and Arkngthand's doors)."""
    own, named = set(), set()
    for name, text in db["SCPT"].items():
        for line in text.replace("\r", "").split("\n"):
            line = line.split(";")[0]
            if re.match(r"(?i)\s*" + MOVE_FUNCS + r"\b", line) and name not in DECORATIVE_MOVERS:
                own.add(name)
            m = re.search(r'(?i)"?([\w \'-]+?)"?\s*->\s*' + MOVE_FUNCS + r"\b", line)
            if m:
                named.add(m.group(1).strip().lower())
    out = set()
    for oid, rec in db["objects"].items():
        if rec.tag in ("NPC_", "CREA"):
            continue
        if oid.lower() in named or (rec.zstr("SCRI") or "").lower() in own:
            out.add(oid.lower())
    return out


class Converter:
    """Shared state for converting every cell of a level: the texture set and caches."""

    def __init__(self, db, arch, textures):
        self.db, self.arch, self.textures = db, arch, textures
        objects = db["objects"]
        self.npcs = NpcBuilder(arch, objects, db["RACE"], [r for r in objects.values() if r.tag == "BODY"], db)
        self.creatures = CreatureBuilder(arch, self.npcs)
        self.nif_cache = {}
        self.skeletons, self.skel_index = [], {}     # shared by every cell (cells/skeletons.skl)
        self.dynamic = moving_objects(db)
        self.toggled = toggled_objects(db)
        # Cell cache: a cell whose references, settings and converter code are unchanged since the last
        # run isn't converted again (out/cell_cache/<data folder>/<file>.json holds what convert returned)
        self.cache_dir = OUT.parent / "cell_cache" / OUT.name
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        tools = Path(__file__).parent
        code = b"".join((tools / f).read_bytes() for f in ("convert_cell.py", "meshes.py", "nif.py", "actors.py",
                        "npc.py", "creature.py", "terrain.py", "textures.py", "mwfiles.py"))
        self.code_hash = hashlib.sha1(code).hexdigest()
        # Skeleton indices are written into .act files: keep the order of earlier runs
        registry = self.cache_dir / "skeletons.json"
        for path in (json.loads(registry.read_text()) if registry.exists() else []):
            self.skeleton(path)
        self.cached = 0
        self.libraries = set()                        # actor library files written this run
        self.library_meshes = {}                      # record id -> its library meshes
        self.actor_skeleton = {}                      # record id -> skeleton path (its library is named after it)

    @staticmethod
    def library_path(rec_id):
        name = re.sub(r"[^a-z0-9_]", "_", rec_id) + ".aml"
        return f"cells/actors/{shard_dir(name)}/{name}"

    def actor_library(self, rec_id, shapes, skeleton):
        """cells/actors/<xx>/<id>.aml with the record's meshes, written once per run; its path."""
        rel = self.library_path(rec_id)
        if rel not in self.libraries:
            out = OUT / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            # (a shape without vertex data for posing can't be animated: left out)
            meshes, texs = [], []
            for s in shapes:
                if "raw" not in s:
                    continue
                meshes.append(actor_mesh(s, skeleton, -1, bake_colors(s, [], np.full(3, NEUTRAL_LIGHT),
                                                                      np.zeros(3))))
            merged = merge_meshes([dict(m, tex_name=texture_out_name(s["tex"]) if s["tex"] else "")
                                   for m, s in zip(meshes, [s for s in shapes if "raw" in s])])
            write_actor_library(out, merged, [m.get("tex_name", "") for m in merged])
            self.libraries.add(rel)
            self.library_meshes[rec_id] = merged
        return rel

    def spawn_library(self, rec):
        """A creature / NPC placed at run time: its library (path) and skeleton index, or None."""
        ref = {"id": rec.id, "pos": (0.0, 0.0, 0.0), "rot": (0.0, 0.0, 0.0), "scale": 1.0}
        shapes, info = (self.npcs if rec.tag == "NPC_" else self.creatures).build(rec, ref)
        if not info:
            return None
        sk = self.skeletons[self.skeleton(info["skeleton"])]
        self.actor_skeleton[rec.id.lower()] = info["skeleton"]
        for s in shapes:
            if s.get("tex") and "raw" in s:
                self.textures.request_shape(s, s["flags"] & (FLAG_BLEND | FLAG_TEST))
        return self.actor_library(rec.id.lower(), shapes, sk), self.skel_index[info["skeleton"]]

    def skeleton(self, path):
        """Index of a shared skeleton (cells/skeletons.skl), added the first time."""
        if path not in self.skel_index:
            self.skel_index[path] = len(self.skeletons)
            self.skeletons.append(skeleton_export(self.npcs.nif(path)))
        return self.skel_index[path]

    def save_cache(self):
        """After the last cell: the skeleton order the cached .act files rely on."""
        paths = sorted(self.skel_index, key=self.skel_index.get)
        (self.cache_dir / "skeletons.json").write_text(json.dumps(paths))

    def convert(self, cell_name, refs, env, spawn):
        """convert_cell, or what it returned last time when nothing about the cell changed."""
        file = env.get("file") or cell_file_name(cell_name)
        base = OUT / "cells" / file
        plain = {k: v for k, v in env.items() if k not in ("terrain", "weather")}
        key = hashlib.sha1(json.dumps([self.code_hash, cell_name, refs, plain, env.get("weather"), spawn],
                                      default=str, sort_keys=True).encode()).hexdigest()
        meta_path = self.cache_dir / (file + ".json")
        if meta_path.exists() and base.with_suffix(".cel").exists() and base.with_suffix(".act").exists():
            meta = json.loads(meta_path.read_text())
            if meta.get("key") == key:
                for path, px, alpha in meta["tex_requests"]:
                    self.textures.request(path, px, alpha)
                for path in meta["skeletons"]:
                    self.skeleton(path)
                self.actor_skeleton.update(meta.get("actor_skeletons", {}))
                self.cached += 1
                return {"refs": meta["refs"], "npcs": [self.db["objects"][i] for i in meta["npc_ids"]],
                        "file": file, "textures": meta["textures"], "geometry_bytes": meta["geometry_bytes"]}
        self.textures.log, self.cell_skeletons, self.cell_actors = [], set(), {}
        out = self.convert_cell(cell_name, refs, env, spawn)
        self.actor_skeleton.update(self.cell_actors)
        meta = {"key": key, "refs": out["refs"], "npc_ids": [r.id.lower() for r in out["npcs"]],
                "textures": out["textures"], "geometry_bytes": out["geometry_bytes"],
                "tex_requests": self.textures.log, "skeletons": sorted(self.cell_skeletons),
                "actor_skeletons": self.cell_actors}
        self.textures.log = None
        meta_path.write_text(json.dumps(meta, default=float))
        return out

    def load_nif(self, model):
        key = model.lower()
        if key not in self.nif_cache:
            try:
                nif = Nif(self.arch.read("meshes\\" + key))
                # A placed object's root node rotation doesn't count (as in the game): some Dwemer
                # corridor pieces carry a 90 degree one, which turned them and left holes in the floor
                for r in nif.roots:
                    b = nif.get(r)
                    if b is not None and "rotation" in b:
                        b["rotation"] = [1, 0, 0, 0, 1, 0, 0, 0, 1]
                self.nif_cache[key] = nif
            except Exception as e:
                print(f"  skip mesh {model}: {e}")
                self.nif_cache[key] = None
        return self.nif_cache[key]

    def interior(self, cell_name, spawn=None):
        cell = self.db["CELL"][cell_name]
        ambi = cell.get("AMBI")
        # Water as OpenMW reads it: the "has water" flag (DATA bit 1), the height from INTV (a whole
        # number, older records) or WHGT (a float), whichever comes last before the references; 0 if neither
        has_water = bool(struct.unpack_from("<I", cell.get("DATA"))[0] & 2)
        water_z = 0.0
        for tag, d in cell.subs:
            if tag == "FRMR":
                break
            if tag == "WHGT":
                water_z = struct.unpack("<f", d)[0]
            elif tag == "INTV":
                water_z = float(struct.unpack("<i", d)[0])
        env = {"interior": True, "ambient": color3(ambi, 0), "sun": color3(ambi, 4), "sun_dir": INTERIOR_SUN_DIR,
               "info": {"ambient": list(ambi[0:4]), "fog": list(ambi[8:12]),
                        "fog_density": struct.unpack_from("<f", ambi, 12)[0],
                        "fog_start": 0.0, "fog_end": 0.0, "water_z": water_z if has_water else 0.0, "water": has_water,
                        "bounds": [0.0] * 4, "interior": True},
               "chunk": INTERIOR_CHUNK}
        return self.convert(cell_name, cell_refs(cell), env, spawn)

    def exterior_cell(self, gx, gy, terrain, walk_bounds, weather, walkable, name, file, spawn=None):
        """One exterior grid cell. Walkable cells get their references, full-resolution terrain and
        collision; horizon cells (around the walkable area) only coarse terrain and water.
        walk_bounds = (x0, y0, x1, y1) world rectangle the player is kept in."""
        cell = self.db["CELL"].get((gx, gy))
        refs = cell_refs(cell) if walkable and cell is not None else []
        ambient, sun = weather["ambient"], weather["sun"]
        fog = [int(round(c * 255)) for c in weather["fog"]] + [255]
        env = {"interior": False, "ambient": ambient, "sun": sun, "sun_dir": EXTERIOR_SUN_DIR,
               "info": {"ambient": [int(round(c * 255)) for c in ambient] + [255], "fog": fog, "fog_density": 1.0,
                        "fog_start": FOG_START, "fog_end": FOG_END, "water_z": 0.0, "water": True,
                        "bounds": list(walk_bounds), "interior": False},
               "chunk": EXTERIOR_CHUNK, "grid": (gx, gy), "terrain": terrain, "walkable": walkable,
               "weather": weather, "file": file}
        return self.convert(name, refs, env, spawn)

    def convert_cell(self, cell_name, refs, env, spawn):
        """Writes cells/<name>.cel and .act; returns {"refs": [...], "npcs": [NPC_ records], "file"}."""
        objects = self.db["objects"]
        ambient, sun, sun_dir = env["ambient"], env["sun"], env["sun_dir"]
        info = dict(env["info"])
        chunk_size = env["chunk"]
        textures = self.textures

        # Lights: position from the mesh's AttachLight node when present
        lights = []
        for ref in refs:
            rec = objects.get(ref["id"].lower())
            if not rec or rec.tag != "LIGH":
                continue
            lhdt = rec.get("LHDT")
            radius = struct.unpack_from("<i", lhdt, 12)[0]
            flags = struct.unpack_from("<i", lhdt, 20)[0]
            if flags & LIGHT_OFF_DEFAULT or radius <= 0:
                continue
            m = ref_matrix(ref)
            model = rec.zstr("MODL")
            nif = self.load_nif(model) if model else None
            attach = find_node(nif, "attachlight") if nif else None
            pos = (m @ attach)[:3, 3] if attach is not None else np.array(ref["pos"])
            color = color3(lhdt, 16) * (-1 if flags & LIGHT_NEGATIVE else 1)
            lights.append({"pos": pos, "radius": float(radius), "color": color})
        print(f"{cell_name}{' ' + str(env['grid']) if 'grid' in env else ''}: {len(refs)} refs, {len(lights)} lights",
              flush=True)

        def chunk_of(s):
            c = (s["pos"].min(axis=0) + s["pos"].max(axis=0)) / 2
            return (int(math.floor(c[0] / chunk_size)), int(math.floor(c[1] / chunk_size)))

        groups, door_groups, col_ranges = {}, {}, {}
        col_pos, col_tris, col_count, actors, npc_recs = [], [], 0, [], []
        npc_models = []
        ref_out = []
        for ri, ref in enumerate(refs):
            rec = objects.get(ref["id"].lower())
            if rec is None and ref["id"].lower() in self.db["LEVC"]:
                rec = resolve_leveled(self.db, ref["id"])
                if rec is not None:
                    ref = dict(ref, id=rec.id)
            entry = {"id": ref["id"], "type": rec.tag if rec else "", "pos": list(ref["pos"]),
                     "rot": list(ref["rot"]), "scale": ref["scale"]}
            for k in ("lock", "key", "count", "soul", "charge"):
                if k in ref:
                    entry[k] = ref[k]
            if "dest" in ref:
                entry["dest"] = {"cell": ref.get("dest_cell"), "pos": list(ref["dest"][:3]), "rot": list(ref["dest"][3:])}
            ref_out.append(entry)
            if rec is None or rec.tag in SKIPPED_TYPES:
                continue
            shapes = []
            if rec.tag in ("NPC_", "CREA"):
                actors.append({"id": rec.id, "name": rec.zstr("FNAM") or rec.id,
                               "pos": ref["pos"], "yaw": ref["rot"][2]})
                npc_recs.append(rec)
                entry["actor"] = len(actors) - 1
                shapes, actor_info = (self.npcs if rec.tag == "NPC_" else self.creatures).build(rec, ref)
                if actor_info:
                    # Animated at run time: kept out of the baked batches
                    aiw = rec.get("AI_W")
                    npc_models.append({"ref": ri, "shapes": shapes, "info": actor_info, "id": rec.id.lower(),
                                       "idle": list(aiw[5:13]) if aiw else [0] * 8})
            else:
                model = rec.zstr("MODL")
                if not model or is_marker(model):
                    continue
                nif = self.load_nif(model)
                if nif is None:
                    continue
                m = ref_matrix(ref)
                can_carry = rec.tag == "LIGH" and struct.unpack_from("<i", rec.get("LHDT"), 20)[0] & LIGHT_CAN_CARRY
                # swinging doors, and objects scripts move or turn (gates, Dwemer doors, platforms): their
                # own mesh drawn where they are now, blocking at run time instead of in the baked collision
                swing_door = (rec.tag == "DOOR" and "dest" not in ref) or ref["id"].lower() in self.dynamic
                if rec.tag in COLLIDING_TYPES and not can_carry and not swing_door:   # swing doors block at run time
                    col = collision_triangles(nif)
                    if col is not None:
                        if ref["id"].lower() in self.toggled:
                            col_ranges[ri] = (sum(len(x) for x in col_tris), len(col[1]))
                        col_pos.append(col[0] @ m[:3, :3].T + m[:3, 3])
                        col_tris.append(col[1] + col_count)
                        col_count += len(col[0])
                shapes = extract_shapes(self.arch, nif, bind_pose=True)
                if swing_door:
                    # Swinging door: kept in object space, drawn with its own transform
                    for s in shapes:
                        s["local_pos"] = s["pos"].copy()
                    world_shapes(shapes, m)
                    entry["door_mesh"] = len(door_groups)
                    door_groups[ri] = shapes
                    shapes = []
                else:
                    world_shapes(shapes, m)
            allpos = [s["pos"] for s in (shapes or door_groups.get(ri, []))]
            if allpos:
                p = np.concatenate(allpos)
                entry["bbox"] = [p.min(0).round(1).tolist(), p.max(0).round(1).tolist()]
            if rec.tag in ("NPC_", "CREA"):
                continue
            detail = 0.0
            if not env["interior"] and allpos and np.linalg.norm(p.max(0) - p.min(0)) < DETAIL_SIZE:
                detail = DETAIL_DISTANCE
            for s in shapes:
                s["ref"] = ri if rec.tag != "STAT" else None
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], chunk_of(s), detail), []).append(s)

        sky = []
        if not env["interior"]:
            terrain, (gx, gy), walkable = env["terrain"], env["grid"], env["walkable"]
            for s in terrain_shapes(self.db, self.arch, terrain, gx, gy, walkable, sun, ambient, textures):
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], s["chunk"]), []).append(s)
            for s in water_shapes(self.arch, terrain, gx, gy, info["water_z"], sun, ambient, textures):
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], s["chunk"]), []).append(s)
            if walkable and terrain.has_land[gy - terrain.y0, gx - terrain.x0]:
                tp, tt = terrain.collision(gx, gy, gx, gy)
                col_pos.append(tp)
                col_tris.append(tt + col_count)
                col_count += len(tp)
        elif info["water"]:
            for s in interior_water_shapes(self.arch, groups, info["water_z"], sun, ambient, textures, chunk_of):
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], s["chunk"], 0.0), []).append(s)

        groups = merge_small_groups(groups)

        # Textures: every use asks for the texel density it needs (textures.py)
        tex_names, tex_index = [], {}

        def use(s, key):
            tex = s["tex"]
            if not tex:
                return
            if tex not in tex_index:
                tex_index[tex] = len(tex_names)
                tex_names.append(textures.request(tex, 0, False))
            if "rgba" not in s:
                textures.request_shape(s, key[1] & (FLAG_BLEND | FLAG_TEST))

        for key, ss in groups.items():
            for s in ss:
                use(s, key)
        for shapes in door_groups.values():
            for s in shapes:
                use(s, (s["tex"], s["flags"]))
        for a in npc_models:
            for s in a["shapes"]:
                use(s, (s["tex"], s["flags"]))

        ranges = {}
        batches = make_batches(groups, tex_index, lights, ambient, sun, sun_dir, ranges)
        for ri, rr in ranges.items():
            ref_out[ri]["ranges"] = rr
        door_meshes = []
        for ri, shapes in door_groups.items():
            g = {}
            for s in shapes:
                g.setdefault((s["tex"], s["flags"], s["alpha_ref"]), []).append(s)
            door_meshes.append({"ref": ri, "batches": make_batches(g, tex_index, lights, ambient, sun, sun_dir)})
        if not env["interior"]:
            sky = sky_batches(self.arch, env["weather"], textures, tex_index, tex_names)

        if spawn:
            feet, yaw = spawn
            info["spawn"], info["yaw"] = [feet[0], feet[1], feet[2] + 124.0], yaw
        else:
            allpos = np.concatenate([b["verts"]["pos"] for b in batches]) if batches else np.zeros((1, 3))
            info["spawn"], info["yaw"] = list((allpos.min(0) + allpos.max(0)) / 2), 0.0

        base = OUT / "cells" / (env.get("file") or cell_file_name(cell_name))
        base.parent.mkdir(parents=True, exist_ok=True)
        collision = (np.concatenate(col_pos), np.concatenate(col_tris)) if col_pos else \
            (np.zeros((3, 3)), np.array([[0, 1, 2]]))
        placed = write_cell(base.with_suffix(".cel"), info, tex_names, batches, collision, actors, door_meshes, sky,
                            col_ranges)
        for ri, (first, n) in placed.items():
            if n:
                ref_out[ri]["col"] = [first, n]

        # Animated actors: shared skeletons; each NPC / creature record's meshes once in a library file
        # (lit evenly), each placement with a tint for the light where it stands
        skeletons, skel_index, act_out = self.skeletons, self.skel_index, []
        for a in npc_models:
            path = a["info"]["skeleton"]
            self.cell_skeletons.add(path)
            sk = skeletons[self.skeleton(path)]
            place = np.asarray(a["info"]["place"])
            self.cell_actors[a["id"]] = path
            act_out.append({"ref": a["ref"], "skeleton": skel_index[path], "place": place, "idle": a["idle"],
                            "lib": self.actor_library(a["id"], a["shapes"], sk),
                            "meshes": self.library_meshes[a["id"]],     # (for the memory estimate below)
                            "tint": light_tint(place[:3, 3] + [0.0, 0.0, 64.0], lights, ambient, sun)})
        write_actor_placements(base.with_suffix(".act"), act_out)
        nv = sum(len(b["verts"]) for b in batches)
        nt = sum(len(b["idx"]) for b in batches) // 3
        size = base.with_suffix(".cel").stat().st_size + base.with_suffix(".act").stat().st_size
        print(f"  {base.name}.cel: {len(batches)} batches, {nv} verts, {nt} tris, {len(door_meshes)} swing doors, "
              f"{len(sky)} sky batches, collision {len(collision[1])} tris, {len(tex_names)} textures, "
              f"{len(act_out)} animated actors; {size // 1024} KB on disk")
        # Linear memory the 3DS needs for this cell's buffers (textures are added by the caller)
        buf = lambda bs: sum(len(b["verts"]) * 16 + len(b["idx"]) * 2 for b in bs)
        geometry = buf(batches) + buf(sky) + sum(buf(d["batches"]) for d in door_meshes) + \
            sum(len(m["pos"]) * VERTEX.itemsize + len(np.asarray(m["tris"]).ravel()) * 2
                for a in act_out for m in a["meshes"])
        return {"refs": ref_out, "npcs": npc_recs, "file": base.name, "textures": tex_names,
                "geometry_bytes": geometry}
