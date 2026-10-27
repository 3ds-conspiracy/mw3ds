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

from mwfiles import OVERLAYS, cell_refs, content_key, fixed_utf8, safe_name
from meshes import (FLAG_BLEND, FLAG_DECAL, FLAG_RISE, FLAG_SCROLL, FLAG_TEST, FLAG_TWO_SIDED,
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
# OpenMW lights reach this far past their radius (its "light bounds multiplier"), fading out over the last quarter
LIGHT_BOUNDS = 1.65


def light_falloff(dist, radius):
    """A point light's strength at a distance, as OpenMW lights the world: radius / (3 x distance), not capped (beside
    the lamp it is several times its colour), faded to nothing from 0.75 to 1 of its bounds (calcAttenuation, fade)."""
    att = radius / (LINEAR_ATTEN * np.maximum(dist, 1.0))
    x = np.clip((dist / (LIGHT_BOUNDS * radius) - 0.75) / 0.25, 0.0, 1.0)
    return att * (1.0 - x * x) ** 2
INTERIOR_SUN_DIR = np.array([-0.15, 0.15, 1.0]) / np.linalg.norm([-0.15, 0.15, 1.0])
# Outdoors the sun's path runs east to west along (400 x orbit, -75, 100) (OpenMW's WeatherManager::update, Morrowind's
# constant): baked from where it is at noon, overhead and to the south. Its east-west part is the side light
# (shaders/cell.v.pica sideLight): each opaque vertex keeps how much it faces east in its colour's alpha, lit at run time from
# the sun's place on the path (and the night light's, which runs back the same way)
EXTERIOR_SUN_DIR = np.array([0.0, -75.0, 100.0]) / np.linalg.norm([0.0, -75.0, 100.0])
LIGHT_NEGATIVE, LIGHT_OFF_DEFAULT = 0x4, 0x20

VERTEX = np.dtype([("pos", "<f4", 3), ("uv", "<f4", 2), ("color", "u1", 4), ("lamp", "<i2")])

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
# Exterior objects are batched by size and not drawn past a distance that grows with it (size x DETAIL_RANGE, the
# smallest tier's cutoff for the rest): clutter under 160 units across goes at 2500, a cart at 5000, a house at
# 10000, the biggest only in the fog. Each tier vanishes at the same size on the screen, about 25 pixels at a full
# view (less as the view shrinks: the game scales these by its view distance). Small things are most of the draw
# calls in a town, and the 3DS's CPU pays for every draw call; the GPU is the same on both consoles
DETAIL_TIERS = ((160.0, 2500.0), (320.0, 5000.0), (640.0, 10000.0))   # (size under, cutoff)


def detail_distance(size):
    """The distance past which an exterior object `size` units across isn't drawn (0: the fog decides)."""
    for under, dist in DETAIL_TIERS:
        if size < under:
            return dist
    return 0.0


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

def point_light(pos, nrm, lights, omni=False):
    """The point lights' light at each vertex: Morrowind's linear attenuation, faded out past the radius."""
    light = np.zeros((len(pos), 3))
    if not len(pos):
        return light
    lo, hi = pos.min(axis=0), pos.max(axis=0)
    for L in lights:
        reach = LIGHT_BOUNDS * L["radius"]
        if np.any(L["pos"] < lo - reach) or np.any(L["pos"] > hi + reach):
            continue
        d = L["pos"] - pos
        dist = np.maximum(np.linalg.norm(d, axis=1), 1.0)
        ndl = np.ones(len(pos)) if omni else np.clip(np.sum(nrm * d, axis=1) / dist, 0, None)
        light += (ndl * light_falloff(dist, L["radius"]))[:, None] * L["color"]
    return light


def shade(s, lights, ambient, sun, sun_dir=INTERIOR_SUN_DIR):
    """A shape's lit colour (rgba 0..1) and the part of its rgb its own glow and the point lights give: the part the
    night outdoors leaves alone (OpenMW: the sun and the ambient light change with the hour, lamps and emissive don't)."""
    if "rgba" in s:                        # terrain, water, sky: colours computed by their builders
        rgba = np.clip(s["rgba"], 0, 1)
        lamp = np.minimum(s["lamp"], rgba[:, :3]) if "lamp" in s else np.zeros((len(rgba), 3))
        return rgba, lamp
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
        color = lamp = emi
    else:
        pos, nrm = s["pos"], s["nrm"]
        omni = s.get("omni")                # (facing the eye: lit from any side, a world spray's particles)
        light = (np.ones(len(pos)) if omni else np.clip(nrm @ sun_dir, 0, None))[:, None] * sun
        lamp = emi + dif * point_light(pos, nrm, lights, omni)
        color = amb * ambient + dif * light + lamp
    rgb = np.nan_to_num(np.clip(color, 0, 1), nan=1.0)
    rgba = np.concatenate([rgb, np.clip(alpha, 0, 1)[:, None]], axis=1)
    return rgba, np.minimum(np.nan_to_num(np.clip(lamp, 0, 1)), rgb)


def side_facing(s):
    """How much each vertex faces east (+1) or west (-1), times its diffuse colour's brightness: what the low sun's light
    adds to it (shaders/cell.v.pica sideLight). 0 for shapes lit otherwise (terrain, water, sky, glow-only, particles)."""
    n = len(s["pos"])
    if "rgba" in s or s.get("omni") or s.get("nrm") is None:
        return np.zeros(n)
    if s["vcp"] and s["vcp"]["lighting_mode"] == 0:
        return np.zeros(n)
    dif = np.tile(np.array(s["mat"]["diffuse"]) if s["mat"] else np.ones(3), (n, 1))
    if s["vcol"] is not None and (s["vcp"]["vertex_mode"] if s["vcp"] else 2) == 2:
        dif = s["vcol"][:, :3]
    return np.clip(s["nrm"][:, 0], -1, 1) * np.clip(dif @ [0.299, 0.587, 0.114], 0, 1)


def side_bytes(f):
    """side_facing's -1..1 as a byte, 0..255 (128: neither way)."""
    return np.clip(np.round((np.asarray(f) + 1.0) * 127.5), 0, 255).astype(np.uint8)


def to_bytes(rgba):
    return (np.nan_to_num(rgba, nan=1.0) * 255 + 0.5).astype(np.uint8)


def bake_colors(s, lights, ambient, sun, sun_dir=INTERIOR_SUN_DIR):
    return to_bytes(shade(s, lights, ambient, sun, sun_dir)[0])


# Outdoors a batch's lamp light rides along: one colour for the batch, and at each vertex a brightness of it in the
# packed position's spare w, x LAMP_SCALE (0..2). shaders/cell.v.pica adds brightness x colour x (1 - the hour's tint) to the
# tinted colour, so night darkens the sun's and the sky's share and leaves the lamps' (one instruction a vertex)
LAMP_SCALE = 16383.0


def lamp_channel(lamps):
    """A batch's lamp light (n x 3, 0..1) as its colour (brightest channel 1) and each vertex's brightness of it
    (int16, x LAMP_SCALE); None and zeros where no lamp reaches."""
    w = np.zeros(len(lamps), dtype=np.int16)
    if not len(lamps) or lamps.max() < 1.0 / 255.0:
        return None, w
    total = lamps.sum(axis=0)
    color = total / total.max()
    k = lamps @ color / (color @ color)
    return color, np.round(np.clip(k, 0, 2) * LAMP_SCALE).astype(np.int16)


# Actor libraries are lit evenly at this level; each placement's tint (x 64 in a byte) scales it to the
# light where it stands: ambient, the sun, and the point lights' reach there
NEUTRAL_LIGHT = 0.5
# A light falls on the side of a body that faces it (OpenMW lights actors by their normals). The sun from above: half
# its light, as seen. A lamp at body height lights a standing figure from the side: the cosine over a cylinder's
# surface averages 1 / pi. Taken whole, a lamp beside someone indoors lit them twice as bright as OpenMW's against a
# room that matched (Socucius Ergalla, Raxle Berne, Yagrum Bagarn side by side)
SUN_SHARE = 0.5
FACING_SHARE = 1.0 / math.pi


def light_tint(pos, lights, ambient, sun):
    """The placement's tint (x 64) and the lamps' share of it (x 64): night outdoors darkens the rest only."""
    lamp = np.zeros(3)
    for L in lights:
        dist = max(float(np.linalg.norm(L["pos"] - pos)), 1.0)
        lamp = lamp + FACING_SHARE * float(light_falloff(dist, L["radius"])) * np.asarray(L["color"])
    light = np.asarray(ambient, dtype=float) + SUN_SHARE * np.asarray(sun, dtype=float) + lamp
    byte = lambda v: [int(c) for c in np.clip(np.round(v / NEUTRAL_LIGHT * 64), 0, 255)]
    tint = byte(light)
    return tint, [min(a, b) for a, b in zip(byte(lamp), tint)]


# ---- cell file ----

def planar(records):
    """(n, size) bytes -> bytes as planes: byte 0 of every record, then byte 1 ... (components/misc/planes.hpp undoes it)"""
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


def write_batches(f, batches, shared=False):
    """Batches of one group (a cell's static batches, one door, or the sky) share one position grid:
    s16 = (pos - offset) / scale, so coplanar terrain layers in different batches quantize identically.
    The step is a power of two and the offset a multiple of it: terrain corners (multiples of 8) then sit
    on every cell's grid, and the engine can rebuild positions near the camera exactly in the GPU's
    24-bit floats (mwrender/renderingmanager.cpp uploadTransform), so neighbouring cells meet without cracks.
    UVs are s16 scaled per batch. 16 bytes per vertex instead of 24. shared (a shared model's batches, format 11): 12
    bytes, no colour (each placement's colours come apart, write_shared)."""
    f.write(struct.pack("<I", len(batches)))
    if not batches:
        return
    allpos = np.concatenate([b["verts"]["pos"] for b in batches]).astype(np.float64)
    lo, hi = allpos.min(axis=0), allpos.max(axis=0)
    scale = 2.0 ** np.ceil(np.log2(np.maximum(hi - lo, 1e-3) / 65534.0))
    while True:
        offset = np.round((lo + hi) / 2.0 / scale) * scale
        over = np.maximum(hi - offset, offset - lo) / scale > 32767.0
        if not over.any():
            break
        scale = np.where(over, scale * 2.0, scale)
    for b in batches:
        v = b["verts"]
        p = v["pos"]
        uv_scale = np.maximum(np.abs(v["uv"]).max(axis=0) if len(v) else np.ones(2), 1e-4) / 32767.0
        f.write(struct.pack("<iBBHII6f8f", b["tex"], b["flags"], b["alpha_ref"], int(b.get("max_dist", 0) // 8),
                            len(v), len(b["idx"]), *p.min(axis=0), *p.max(axis=0),
                            *offset, *scale, *uv_scale))
        # Format 8: u8 frames, columns, rows, frames a second of a looping flipbook (0 none): the frame steps its
        # texture by a cell each 1 / rate s (a world spray's sheet, particles.loop_flipbook)
        flip = b.get("flip")
        f.write(struct.pack("<4B", *(flip[:3] + (int(flip[3]),)) if flip else (0, 0, 0, 0)))
        # Format 9: s16 u, v the texture slides by, 1/256 repeat a second (a model's NiUVController; 0 none). V turns
        # over with the coordinates (DirectX V down, PICA V up)
        su, sv = b.get("scroll") or (0.0, 0.0)
        f.write(struct.pack("<2h", int(np.clip(round(su * 256), -32767, 32767)), int(np.clip(round(-sv * 256), -32767, 32767))))
        # Format 10: u8 rgb the lamp light's colour, the vertices' w its brightness (lamp_channel; 0 none); then 1 when
        # the colours' alpha holds the side light (side_facing), 0 when it is the alpha
        lc = b.get("lamp_color")
        f.write(bytes(([int(round(c * 255)) for c in lc] if lc is not None else [0, 0, 0]) + [1 if b.get("side") else 0]))
        fields = [("pos", "<i2", 4), ("uv", "<i2", 2)] + ([] if shared else [("color", "u1", 4)])
        q = np.zeros(len(v), dtype=fields)
        q["pos"][:, :3] = np.clip(np.round((p - offset) / scale), -32767, 32767)
        if "lamp" in v.dtype.names:
            q["pos"][:, 3] = v["lamp"]
        q["uv"] = np.clip(np.round(v["uv"] / uv_scale), -32767, 32767)
        if not shared:
            q["color"] = v["color"]
        # Format 7: positions as differences from the previous vertex, then everything as byte planes
        rec = q.view(np.uint8).reshape(-1, q.dtype.itemsize).copy()
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


def write_cell(path, info, textures, batches, collision, actors, door_meshes, sky, col_ranges=None, shared=None):
    """Layout (little endian):
       'MWC1' u32 version=10 u32 flags (bit0 interior, bit1 water plane, bit2 sky)
       f32 spawn[3] f32 yaw f32 pitch
       u8 ambient[4] u8 fog[4] f32 fog_density
       f32 fog_start f32 fog_end (0 = no fog) f32 water_z
       f32 bounds[4] min x, min y, max x, max y of the walkable area (all 0 = unbounded)
       u32 num_textures, then 64-byte zero-padded names (relative to data/textures)
       u32 num_batches, then per batch:
         i32 texture (-1 none) u8 flags u8 alpha_ref u16 max_distance / 8 (0 = any) u32 num_vertices u32 num_indices
         f32 bbox_min[3] f32 bbox_max[3] f32 pos_offset[3] f32 pos_scale[3] f32 uv_scale[2]
         vertices (16 bytes: s16 pos[4] (w: the lamp light's brightness, format 10), s16 uv[2], u8 rgba[4];
         pos = offset + s16 * scale, uv = s16 * uv_scale), u16 indices, pad to 4 bytes
         flags: 1 blend, 2 alpha test, 4 two-sided, 8 clamp, 16 additive,
                32 decal (coplanar overlay: equal depth passes, no depth write), 64 scrolling UVs,
                128 rising UVs (smoke, steam)
         format 8: u8 flipbook frames, columns, rows, frames a second; format 9: s16 scroll u, v (1/256 repeat a second);
         format 10: u8 lamp colour rgb, pad (0: no lamp light; outdoors only)
       collision (see write_collision): u32 num_vertices, f32 offset[3], f32 scale[3], s16 pos[3]... (pad 4),
         u32 num_triangles, u32 widths (byte 0 triangle index bytes, byte 1 grid list bytes: 2 or 4),
         idx[3]... (pad 4), f32 grid_origin[2], f32 grid_cell, u32 nx, u32 ny, u32 cell_start[nx*ny+1],
         cell_tris[] (pad 4)
       actors: u32 count, then per actor: char id[32], char name[32], f32 pos[3], f32 yaw,
         f32 radius, f32 height
       swing doors: u32 count, then per door: u32 ref index, batches as above in object space
       sky: batches as above around the origin, drawn centred on the camera without fog
       format 11 (only a cell with shared models; any other is written as format 10): u32 count, then per shared model
         (share_models): its batches as above in the model's own space, 12-byte vertices (s16 pos[4], s16 uv[2]: no
         colour), u32 placements, per placement f32 matrix[12] (3 x 4 rows, model -> world) f32 bbox_min[3]
         f32 bbox_max[3] f32 max_distance u32 ref index (not drawn while it is disabled), then u32 colour sets (1: one
         for every placement, SHARE_ONE_LIGHT; else one a placement) and the sets (u8 rgba a vertex of the model's
         batches, in order, set after set) as byte planes
    """
    flags = (CELL_INTERIOR if info["interior"] else 0) | (CELL_WATER if info["water"] else 0) \
        | (CELL_SKY if sky else 0)
    with open(path, "wb") as f:
        f.write(b"MWC1" + struct.pack("<II", 11 if shared else 10, flags))
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
            f.write(fixed_utf8(a["id"], 32))
            f.write(fixed_utf8(a["name"], 32))
            f.write(struct.pack("<6f", *a["pos"], a["yaw"], ACTOR_RADIUS, ACTOR_HEIGHT))
        f.write(struct.pack("<I", len(door_meshes)))
        for d in door_meshes:
            f.write(struct.pack("<I", d["ref"]))
            write_batches(f, d["batches"])
        write_batches(f, sky)
        if shared:
            write_shared(f, shared)
    return ranges


def write_shared(f, shared):
    """Format 11's shared models (write_cell)."""
    f.write(struct.pack("<I", len(shared)))
    for sm in shared:
        write_batches(f, sm["batches"], shared=True)
        f.write(struct.pack("<I", len(sm["places"])))
        for pl in sm["places"]:
            f.write(struct.pack("<12f3f3ffI", *np.asarray(pl["m"], dtype=float)[:3, :4].ravel(), *pl["bmin"], *pl["bmax"],
                                pl["max_dist"], pl["ref"]))
        sets = [sm["colors"]] if sm["colors"] is not None else [pl["colors"] for pl in sm["places"]]
        f.write(struct.pack("<I", len(sets)))
        f.write(planar(np.concatenate(sets)))


def shared_bytes(shared):
    """Linear memory the 3DS holds for shared models: their vertices and indices once, each placement's colours."""
    return sum(sum(len(b["verts"]) * 12 + len(b["idx"]) * 2 for b in sm["batches"])
               + (1 if sm["colors"] is not None else len(sm["places"])) * sum(len(b["verts"]) for b in sm["batches"]) * 4
               for sm in shared)


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


def make_batches(groups, tex_index, lights, ambient, sun, sun_dir, ranges=None, split=False):
    """Merges shape groups into batches (opaque, then alpha-tested, then blended; stable
    otherwise, so terrain layers keep their order). Group keys start with the render key
    (tex, flags, alpha_ref). With `ranges`, records where each shape's triangles land:
    ranges[ref] += [batch, first, count]. split (outdoors): each batch carries its lamp light (lamp_channel)."""
    batches = []
    order = lambda k: (bool(k[1] & FLAG_BLEND), bool(k[1] & FLAG_TEST))
    for key in sorted(groups, key=order):
        tex, flags, alpha_ref = key[:3]
        max_dist = key[4] if len(key) > 4 else 0
        scroll = key[5] if len(key) > 5 else (0.0, 0.0)
        # (a looping flipbook's frames, columns, rows and rate: every spray in the batch shares its sheet)
        loop = next((s["loop"][2] for s in groups[key] if s.get("loop")), None)
        cur_v, cur_i, cur_l, cur_s, base, first = [], [], [], [], 0, 0
        # outdoors, opaque batches take the side light in their colours' alpha (blended and alpha-tested ones need it)
        side = split and not flags & (FLAG_BLEND | FLAG_TEST)

        def close():
            verts = np.concatenate(cur_v)
            color = None
            if split:
                color, verts["lamp"] = lamp_channel(np.concatenate(cur_l))
            if side:
                verts["color"][:, 3] = side_bytes(np.concatenate(cur_s))
            batches.append({"verts": verts, "idx": np.concatenate(cur_i),
                            "tex": tex_index.get(tex, -1), "flags": flags, "alpha_ref": alpha_ref,
                            "max_dist": max_dist, "flip": loop, "scroll": scroll, "lamp_color": color,
                            "side": side})

        for s in groups[key]:
            n = len(s["pos"])
            if base + n > 65535 and cur_v:
                close()
                cur_v, cur_i, cur_l, cur_s, base, first = [], [], [], [], 0, 0
            v = np.zeros(n, dtype=VERTEX)
            v["pos"] = s["local_pos"] if "local_pos" in s else s["pos"]
            v["uv"] = s["uv"] * [1.0, -1.0] + [0.0, 1.0]   # DirectX V (0 = top) -> PICA V (0 = bottom)
            rgba, lamp = shade(s, lights, ambient, sun, sun_dir)
            v["color"] = to_bytes(rgba)
            cur_v.append(v)
            cur_l.append(lamp)
            if side:
                cur_s.append(side_facing(s))
            cur_i.append(np.asarray(s["tris"]).ravel() + base)
            if ranges is not None and s.get("ref") is not None:
                ranges.setdefault(s["ref"], []).append([len(batches), first, len(np.asarray(s["tris"]).ravel())])
            first += len(np.asarray(s["tris"]).ravel())
            base += n
        if cur_v:
            close()
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


# Shared models (format 11). In the 3 x 3 spots heavier than any of Vvardenfell's (level.py SHARE_WINDOW_VERTICES:
# Solstheim's pine forests, a spot's trees about 30 MB as batches against a 24 MB budget for all of linear memory), a
# static placed several times in a cell keeps its vertices once, in its own space, and each placement only its baked
# colours (4 bytes a vertex instead of 16, and no indices): drawn once a placement with its own transform. Each
# placement is a draw call per batch of the model, so only models whose draws are big enough to be worth one
SHARE_TYPES = {"STAT", "ACTI"}  # (Raven Rock's trees are activators a script clears as the colony grows)
# Trees and plants (a model named flora_*) take one baked light for all their placements in a cell: the model lit as it
# stands unturned, by the cell's ambient light and the noon sun. A tree outdoors hardly differs in its light from
# place to place; each placement's own (its turn decides which side the noon sun and the low sun's side light fall on)
# took 4 bytes a vertex a placement, most of a forest's shared models (spec/performance-tradeoffs.md). Others (rocks,
# walls) keep each placement's colours. A placement a lamp lights stays baked either way
SHARE_ONE_LIGHT = "flora"
SHARE_MIN_PLACES = 3            # placements of the model in the cell
SHARE_MIN_VERTICES = 256        # vertices a shared draw, on average over the model's batches
# Kept in the cell's batches: terrain-like overlays, moving or sliding textures, particle sprays. (Blended shapes are
# shared: the pines' needles are blended and alpha tested, each placement's sorted back to front with the rest)
SHARE_EXCLUDED_FLAGS = FLAG_DECAL | FLAG_SCROLL | FLAG_RISE


def shareable(s):
    """A model's shape whose vertices a shared model can hold: plain lit geometry."""
    return not (s["flags"] & SHARE_EXCLUDED_FLAGS) and not s.get("scroll") and not s.get("loop") \
        and not s.get("omni") and "rgba" not in s


def share_models(candidates, groups, lights, ambient, sun, sun_dir, chunk_of):
    """Picks the models of the cell that share one copy of their vertices (SHARE_MIN_PLACES, SHARE_MIN_VERTICES) among
    `candidates` (model -> its placements: {"ri", "m", "shapes" in world space, "obj" their positions in the model's
    space, "detail"}). A placement a lamp lights stays baked: a shared model has no lamp channel, and the night would
    darken its lamp light. A placement keeps its reference: one a script disables (Raven Rock's trees, cleared as the
    colony grows) isn't drawn while it is, as OpenMW hides it (baked statics keep only their collision apart). Everything not shared goes into `groups` as the cell's other shapes do. Returns the shared
    models: {"batches": the model's, "places": [{"m", "bmin", "bmax", "max_dist", "ref", "colors"}], "colors" (one set
    for every placement, SHARE_ONE_LIGHT; else None and each placement has its own), "baked_bytes"}."""
    def bake(pl, shapes=None):
        for sh in (pl["shapes"] if shapes is None else shapes):
            groups.setdefault((sh["tex"], sh["flags"], sh["alpha_ref"], chunk_of(sh), pl["detail"],
                               sh.get("scroll") or (0.0, 0.0)), []).append(sh)

    out = []
    for model, places in candidates.items():
        idx = [i for i, sh in enumerate(places[0]["shapes"]) if shareable(sh)]
        kept = []
        for pl in places:
            ok = len(pl["shapes"]) == len(places[0]["shapes"]) and bool(idx)
            if ok:
                lamp = np.concatenate([shade(pl["shapes"][i], lights, ambient, sun, sun_dir)[1] for i in idx])
                ok = lamp.max() < 1.0 / 255.0
            (kept if ok else []).append(pl)
            if not ok:
                bake(pl)
        # The model's batches: its shareable shapes merged by render key, as make_batches does a cell's
        keys = []
        for i in idx:
            sh = places[0]["shapes"][i]
            k = (sh["tex"], sh["flags"], sh["alpha_ref"])
            if k not in keys:
                keys.append(k)
        nverts = sum(len(places[0]["obj"][i]) for i in idx)
        if len(kept) < SHARE_MIN_PLACES or nverts < SHARE_MIN_VERTICES * max(len(keys), 1):
            for pl in kept:
                bake(pl)
            continue
        order = lambda k: (bool(k[1] & FLAG_BLEND), bool(k[1] & FLAG_TEST))
        keys.sort(key=order)
        members = [[i for i in idx if (places[0]["shapes"][i]["tex"], places[0]["shapes"][i]["flags"],
                                       places[0]["shapes"][i]["alpha_ref"]) == k] for k in keys]
        batches = []
        for k, mem in zip(keys, members):
            v = np.zeros(sum(len(places[0]["obj"][i]) for i in mem), dtype=VERTEX)
            tris, base, at = [], 0, 0
            for i in mem:
                sh = places[0]["shapes"][i]
                n = len(places[0]["obj"][i])
                v["pos"][at:at + n] = places[0]["obj"][i]
                v["uv"][at:at + n] = sh["uv"] * [1.0, -1.0] + [0.0, 1.0]     # DirectX V (0 = top) -> PICA V (0 = bottom)
                tris.append(np.asarray(sh["tris"]).ravel() + at)
                at += n
            batches.append({"verts": v, "idx": np.concatenate(tris), "tex_path": k[0], "tex": -1, "flags": k[1],
                            "alpha_ref": k[2], "max_dist": 0, "flip": None, "scroll": (0.0, 0.0), "lamp_color": None,
                            "side": not k[1] & (FLAG_BLEND | FLAG_TEST), "shapes": [places[0]["shapes"][i] for i in mem]})
        one_light = SHARE_ONE_LIGHT in model.replace("/", "\\").rsplit("\\", 1)[-1]
        sm = {"batches": batches, "places": [], "baked_bytes": 0, "colors": None}

        def colours(shapes):
            cols = []
            for b, mem in zip(batches, members):
                for i in mem:
                    c = to_bytes(shade(shapes[i], lights, ambient, sun, sun_dir)[0])
                    if b["side"]:
                        c[:, 3] = side_bytes(side_facing(shapes[i]))
                    cols.append(c)
            return np.concatenate(cols)

        if one_light:
            # (the model as it stands in its own space: no lamp reaches a shared placement)
            sm["colors"] = colours([dict(sh, pos=places[0]["obj"][i], nrm=places[0]["objn"][i])
                                    for i, sh in enumerate(places[0]["shapes"])])
        for pl in kept:
            wp = np.concatenate([pl["shapes"][i]["pos"] for i in idx])
            sm["places"].append({"m": pl["m"], "bmin": wp.min(axis=0).tolist(), "bmax": wp.max(axis=0).tolist(),
                                 "max_dist": float(pl["detail"]), "ref": pl["ri"],
                                 "colors": None if one_light else colours(pl["shapes"])})
            sm["baked_bytes"] += sum(len(pl["shapes"][i]["pos"]) * 16 + len(np.asarray(pl["shapes"][i]["tris"]).ravel()) * 2
                                     for i in idx)
            bake(pl, [sh for i, sh in enumerate(pl["shapes"]) if i not in idx])
        out.append(sm)
    return out


def cell_file_name(cell_name):
    return safe_name(cell_name)


# ---- exterior extras ----

def terrain_shapes(db, arch, terrain, gx, gy, full, sun, ambient, textures, lights=()):
    """Terrain chunk layers of exterior cell (gx, gy) as pre-coloured shapes, keyed by chunk.
    full: every vertex (walkable cells); else every second one plus a skirt (horizon cells).
    lights: the lamps that light the ground (their share is the shapes' "lamp", make_batches)."""
    shapes = []
    per_cell = int(CELL_SIZE // EXTERIOR_CHUNK)
    tx, ty = gx - terrain.x0, gy - terrain.y0
    if not terrain.has_land[ty, tx]:
        return shapes
    for cy in range(ty * per_cell, (ty + 1) * per_cell):
        for cx in range(tx * per_cell, (tx + 1) * per_cell):
            layers = terrain.chunk(cx, cy, 1 if full else 2, EXTERIOR_SUN_DIR, sun, ambient, skirt=not full,
                                   lamp=(lambda p, n: point_light(p, n, lights)) if lights else None)
            for k, (value, pos, uv, color, tris) in enumerate(layers):
                path = ltex_path(db, arch, value)
                textures.request(path, texels_needed(TILE_SIZE), False)
                flags = FLAG_TWO_SIDED | (FLAG_BLEND | FLAG_DECAL if k else 0)
                shapes.append({"pos": pos, "uv": uv, "rgba": color[:, :4], "lamp": color[:, 4:], "tris": tris,
                               "tex": path, "flags": flags, "alpha_ref": 0, "chunk": ("terrain", cx, cy)})
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


def interior_water_shapes(arch, groups, water_z, sun, ambient, textures, chunk_of, everywhere=False):
    """An interior's water: a quad for every square of the floor plan where some triangle dips below
    the water level, as the exteriors' water quads (same texture, colour and scrolling). everywhere: a square over
    the whole floor plan, for a cell whose scripts raise and lower the water (SetWaterLevel: the engine moves the
    surface; the floors hide what lies under them)."""
    tile = WATER_TILE
    tiles = set()
    for shapes in groups.values():
        for s in shapes:
            pos, tris = s["pos"], np.asarray(s["tris"]).reshape(-1, 3)
            if not len(tris):
                continue
            tp = pos[tris]
            low = np.ones(len(tp), dtype=bool) if everywhere else tp.min(axis=1)[:, 2] < water_z
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
    sky colour overhead) and a scrolling cloud layer that fades out towards the horizon. The gradient is two white
    domes, the fog's (opaque) and the sky's (its share, rising overhead), which the engine colours with the weather
    of the hour (mwrender/renderingmanager.cpp: OpenMW's sky in the weather's own colours)."""
    seg = 16
    rings = [-30, -5, 0, 8, 20, 40, 65, 90]

    def dome(elevations, color_of, alpha_of, reach=None):
        pos, uv, rgba = [], [], []
        for e in elevations:
            er = math.radians(e)
            for i in range(seg + 1):
                a = 2 * math.pi * i / seg
                p = np.array([math.cos(er) * math.sin(a), math.cos(er) * math.cos(a), math.sin(er)]) * SKY_RADIUS
                pos.append(p)
                uv.append(p[:2] / SKY_RADIUS * 1.5 if reach is None else
                          np.array([math.sin(a), math.cos(a)]) * reach(e))
                rgba.append(np.append(color_of(e), alpha_of(e)))
        tris = []
        for r in range(len(elevations) - 1):
            for i in range(seg):
                a, b = r * (seg + 1) + i, (r + 1) * (seg + 1) + i
                tris += [a, a + 1, b + 1, a, b + 1, b]
        return np.array(pos), np.array(uv), np.array(rgba), np.array(tris)

    def share(e):
        return float(np.clip(e / 40.0, 0, 1) ** 0.7)

    shapes = []
    pos, uv, rgba, tris = dome(rings, lambda e: np.ones(3), lambda e: 1.0)
    shapes.append(({"pos": pos, "uv": uv, "rgba": rgba, "tris": tris}, None, FLAG_TWO_SIDED))
    pos, uv, rgba, tris = dome(rings, lambda e: np.ones(3), share)
    shapes.append(({"pos": pos, "uv": uv, "rgba": rgba, "tris": tris}, None, FLAG_BLEND | FLAG_TWO_SIDED))
    cloud = "textures\\" + re.sub(r"\.[^.]+$", ".dds", weather["clouds"].lower())
    if arch.exists(cloud):
        # The picture lies as on Morrowind's cloud dome (sky_clouds_01.nif), a flattened one: 1000 wide, 292 high at the
        # top and 85 at the rim, the picture laid flat across it (2.5 repeats from the middle to the rim). Seen from its
        # middle, little of the picture is overhead and most of it low down, so the clouds shrink towards the horizon.
        # The dome's width at each height (elevation as seen from the middle, degrees: distance out of 1000)
        flat_dome = ([4.9, 15.1, 27.4, 44.9, 90.0], [1000.0, 688.0, 462.0, 275.0, 0.0])
        cloud_rings = [3, 6, 10, 15, 20, 27, 35, 45, 60, 75, 90]
        pos, uv, rgba, tris = dome(cloud_rings, lambda e: np.ones(3),
                                   lambda e: float(np.clip((e - 3) / 25.0, 0, 1)) * 0.9,
                                   lambda e: float(np.interp(e, *flat_dome)) / 1000.0 * 2.5)
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
    # ... and objects whose own script switches them with a bare Enable / Disable / SetDelete, or plays them a death
    # group (Bamz-Amschend's cave-in: its collision goes when it is blown up) (Tribunal's Abandoned
    # Crypt boulders and rockslide disable themselves: their collision stayed baked in, a wall where they were)
    own = set()
    for name, text in db["SCPT"].items():
        for line in (text or "").replace("\r", "").split("\n"):
            if re.match(r"(?i)\s*(enable|disable|setdelete\b|playgroup\s*,?\s*\"?death)", line.split(";")[0]):
                own.add(name)
                break
    names |= {oid for oid, rec in db["objects"].items() if (rec.zstr("SCRI") or "").lower() in own}
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
        self.creatures = CreatureBuilder(arch, self.npcs, db)
        self.nif_cache = {}
        self.skeletons, self.skel_index = [], {}     # shared by every cell (cells/skeletons.skl)
        self.dynamic = moving_objects(db)
        from particles import animated_collision_objects, world_fx_kinds
        self.world_fx = world_fx_kinds(db, arch)
        self.toggled = toggled_objects(db)
        self.anim_col = animated_collision_objects(db, arch)
        # Scripts that move an interior's water (Tribunal's sewers: SetWaterLevel / ModWaterLevel)
        self.water_scripts = {name for name, src in db["SCPT"].items()
                              if re.search(r"\b(set|mod)waterlevel\b", src or "", re.I)}
        # Cell cache: a cell whose references, settings and converter code are unchanged since the last
        # run isn't converted again (out/cell_cache/<data folder>/<file>.json holds what convert returned)
        self.cache_dir = OUT.parent / "cell_cache" / OUT.name
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        tools = Path(__file__).parent
        code = b"".join((tools / f).read_bytes() for f in ("convert_cell.py", "meshes.py", "nif.py", "actors.py",
                        "npc.py", "creature.py", "terrain.py", "textures.py", "mwfiles.py",
                        "particles.py", "npcstats.py"))
        # (and the mod folders laid over Data Files: another overlay is other meshes; and the plugins: a plugin's
        # record can give an object another mesh without touching the cell)
        code += ";".join(str(d) for d in OVERLAYS).encode() + content_key().encode()
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
        name = safe_name(rec_id) + ".aml"
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
                for gname in meta.get("generated", []):
                    from PIL import Image
                    img = Image.open(self.cache_dir / "loops" / (gname + ".png"))
                    img.load()
                    self.textures.generated[gname] = img
                self.actor_skeleton.update(meta.get("actor_skeletons", {}))
                self.cached += 1
                return {"refs": meta["refs"], "npcs": [self.db["objects"][i] for i in meta["npc_ids"]],
                        "file": file, "textures": meta["textures"], "geometry_bytes": meta["geometry_bytes"]}
        self.textures.log, self.cell_skeletons, self.cell_actors, self.cell_generated = [], set(), {}, set()
        out = self.convert_cell(cell_name, refs, env, spawn)
        self.actor_skeleton.update(self.cell_actors)
        meta = {"key": key, "refs": out["refs"], "npc_ids": [r.id.lower() for r in out["npcs"]],
                "textures": out["textures"], "geometry_bytes": out["geometry_bytes"],
                "tex_requests": self.textures.log, "skeletons": sorted(self.cell_skeletons),
                "generated": sorted(self.cell_generated),
                "actor_skeletons": self.cell_actors}
        self.textures.log = None
        meta_path.write_text(json.dumps(meta, default=float))
        return out

    def load_nif(self, model):
        key = model.lower()
        if key not in self.nif_cache:
            try:
                nif = Nif(self.arch.read("meshes\\" + key))
                # A placed object's root node transform doesn't count (as in the game: the reference's own
                # position and turn stand in for it): some Dwemer corridor pieces carry a 90 degree rotation,
                # which turned them and left holes in the floor, and ex_dae_ruin_01 a 1018 unit drop that
                # buried the Daedric ruins' tower base and walled the exit spot of their doors in solid stone
                # (an anvil's -32..32 box sits on its repair hammer at +32 only without its root's 32). A root that is
                # the shape itself keeps its offset, as OpenMW draws it: Contain_crate_01 is one shape moved down 32,
                # and without it Shushishi's crates floated 32 over the platform's deck
                for r in nif.roots:
                    b = nif.get(r)
                    if b is not None and "rotation" in b and b["type"] not in ("NiTriShape", "NiTriStrips"):
                        b["rotation"] = [1, 0, 0, 0, 1, 0, 0, 0, 1]
                        b["translation"] = [0.0, 0.0, 0.0]
                        b["scale"] = 1.0
                self.nif_cache[key] = nif
            except Exception as e:
                print(f"  skip mesh {model}: {e}")
                self.nif_cache[key] = None
        return self.nif_cache[key]

    def interior(self, cell_name, spawn=None, weather=None):
        """An interior. One flagged to behave as an exterior (DATA 0x80, Tribunal's Mournhold: OpenMW's QuasiEx) is lit
        as the exteriors are, by `weather` (the level's), its lamps kept apart for the night, with a sky dome and the
        exteriors' fog; it stays an interior otherwise."""
        cell = self.db["CELL"][cell_name]
        # (Mournhold's sky cells have no AMBI: OpenMW's mood then is all zero, the weather lights them)
        ambi = cell.get("AMBI") or bytes(12) + struct.pack("<f", 0.0)
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
        if struct.unpack_from("<I", cell.get("DATA"))[0] & 0x80 and weather is not None:
            env.update({"sky": True, "ambient": weather["ambient"], "sun": weather["sun"], "sun_dir": EXTERIOR_SUN_DIR,
                        "weather": weather})
            env["info"].update({"fog_start": FOG_START, "fog_end": FOG_END,
                                "fog": [int(round(c * 255)) for c in weather["fog"]] + [255], "fog_density": 1.0})
        refs = cell_refs(cell)
        if has_water and self.water_scripts and any(
                ((self.db["objects"].get((r.get("id") or "").lower()) or None) is not None
                 and (self.db["objects"][(r.get("id") or "").lower()].zstr("SCRI") or "").lower() in self.water_scripts)
                for r in refs):
            env["water_moves"] = True
        return self.convert(cell_name, refs, env, spawn)

    def exterior_cell(self, gx, gy, terrain, walk_bounds, weather, walkable, name, file, spawn=None, share=False):
        """One exterior grid cell. Walkable cells get their references, full-resolution terrain and
        collision; horizon cells (around the walkable area) only coarse terrain and water.
        walk_bounds = (x0, y0, x1, y1) world rectangle the player is kept in. share: a cell of a heavy spot (level.py
        heavy_spots), its repeated statics shared (share_models)."""
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
        if share:
            env["share"] = True          # (only then: other cells' cache keys stay as they were)
        return self.convert(name, refs, env, spawn)

    def lights_of(self, refs):
        """The references' lights (on by default): position from the mesh's AttachLight node when present."""
        objects = self.db["objects"]
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
        return lights

    def convert_cell(self, cell_name, refs, env, spawn):
        """Writes cells/<name>.cel and .act; returns {"refs": [...], "npcs": [NPC_ records], "file"}."""
        objects = self.db["objects"]
        ambient, sun, sun_dir = env["ambient"], env["sun"], env["sun_dir"]
        info = dict(env["info"])
        chunk_size = env["chunk"]
        textures = self.textures

        lights = self.lights_of(refs)
        if not env["interior"] and env["walkable"]:
            # A lamp by the edge of the next cell lights this one's ground and walls too (as far as it reaches)
            gx, gy = env["grid"]
            lo, hi = np.array([gx, gy]) * CELL_SIZE, np.array([gx + 1, gy + 1]) * CELL_SIZE
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    other = self.db["CELL"].get((gx + dx, gy + dy)) if dx or dy else None
                    if other is None:
                        continue
                    for L in self.lights_of([r for r in cell_refs(other) if not r.get("deleted")]):
                        reach = LIGHT_BOUNDS * L["radius"]
                        if np.all(L["pos"][:2] > lo - reach) and np.all(L["pos"][:2] < hi + reach):
                            lights.append(L)
        print(f"{cell_name}{' ' + str(env['grid']) if 'grid' in env else ''}: {len(refs)} refs, {len(lights)} lights",
              flush=True)

        def chunk_of(s):
            c = (s["pos"].min(axis=0) + s["pos"].max(axis=0)) / 2
            return (int(math.floor(c[0] / chunk_size)), int(math.floor(c[1] / chunk_size)))

        groups, door_groups, col_ranges = {}, {}, {}
        candidates = {}                  # (env "share") model -> its placements that may share one copy (share_models)
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
            fx_box = None
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
                    # (a Clockwork City door: its moving collision kept apart, last, for the engine to switch as it
                    # opens and shuts; its frame stays baked in)
                    moves = ref["id"].lower() in self.anim_col
                    col = collision_triangles(nif, parts=moves)
                    if col is not None:
                        if moves:
                            col_ranges[ri] = (sum(len(x) for x in col_tris) + col[2], len(col[1]) - col[2])
                            col = col[:2]
                        elif ref["id"].lower() in self.toggled:
                            col_ranges[ri] = (sum(len(x) for x in col_tris), len(col[1]))
                        col_pos.append(col[0] @ m[:3, :3].T + m[:3, 3])
                        col_tris.append(col[1] + col_count)
                        col_count += len(col[0])
                shapes = extract_shapes(self.arch, nif, bind_pose=True, loops=True)
                # (a world object drawn as its particle piece at run time, particles.WORLD_FX: its old crossed quads
                # left out, or all of it when it is animated; its collision stays)
                wfx = self.world_fx.get(ref["id"].lower())
                if wfx is not None:
                    # (its box stays: the crosshair finds it by the box, and a pile drawn as a piece at run time has no
                    # shapes left to take one from: Bamz-Amschend's cave-in could not be aimed at, so the satchel never
                    # went on it in a run that is played, not scripted)
                    if wfx and shapes:
                        fp = np.concatenate([s["pos"] for s in shapes]) @ m[:3, :3].T + m[:3, 3]
                        fx_box = [fp.min(0).round(1).tolist(), fp.max(0).round(1).tolist()]
                    shapes = [] if wfx else [s for s in shapes if "raw" in s]
                if swing_door:
                    # Swinging door: kept in object space, drawn with its own transform
                    for s in shapes:
                        s["local_pos"] = s["pos"].copy()
                    world_shapes(shapes, m)
                    entry["door_mesh"] = len(door_groups)
                    door_groups[ri] = shapes
                    shapes = []
                else:
                    # (a static of a heavy spot: its shapes in the model's own space too, for share_models)
                    if env.get("share") and rec.tag in SHARE_TYPES and wfx is None and shapes:
                        candidates.setdefault(model.lower(), []).append(
                            {"ri": ri, "m": m, "shapes": shapes, "obj": [s["pos"].copy() for s in shapes],
                             "objn": [s["nrm"].copy() for s in shapes]})
                    world_shapes(shapes, m)
            allpos = [s["pos"] for s in (shapes or door_groups.get(ri, []))]
            if allpos:
                p = np.concatenate(allpos)
                entry["bbox"] = [p.min(0).round(1).tolist(), p.max(0).round(1).tolist()]
            elif fx_box is not None:
                entry["bbox"] = fx_box
            if rec.tag in ("NPC_", "CREA"):
                continue
            detail = 0.0
            if not env["interior"] and allpos:
                detail = detail_distance(float(np.linalg.norm(p.max(0) - p.min(0))))
            for s in shapes:
                s["ref"] = ri if rec.tag != "STAT" else None
            if rec.tag in SHARE_TYPES and candidates.get(model.lower(), [{}])[-1].get("ri") == ri:
                candidates[model.lower()][-1]["detail"] = detail
                continue                 # (share_models decides where its shapes go)
            for s in shapes:
                # (a sliding texture keeps its own batch: the rate is the batch's)
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], chunk_of(s), detail, s.get("scroll") or (0.0, 0.0)),
                                  []).append(s)

        shared = share_models(candidates, groups, lights, ambient, sun, sun_dir, chunk_of) if candidates else []

        sky = []
        if not env["interior"]:
            terrain, (gx, gy), walkable = env["terrain"], env["grid"], env["walkable"]
            for s in terrain_shapes(self.db, self.arch, terrain, gx, gy, walkable, sun, ambient, textures, lights):
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], s["chunk"]), []).append(s)
            for s in water_shapes(self.arch, terrain, gx, gy, info["water_z"], sun, ambient, textures):
                groups.setdefault((s["tex"], s["flags"], s["alpha_ref"], s["chunk"]), []).append(s)
            if walkable and terrain.has_land[gy - terrain.y0, gx - terrain.x0]:
                tp, tt = terrain.collision(gx, gy, gx, gy)
                col_pos.append(tp)
                col_tris.append(tt + col_count)
                col_count += len(tp)
        elif info["water"]:
            for s in interior_water_shapes(self.arch, groups, info["water_z"], sun, ambient, textures, chunk_of,
                                           env.get("water_moves", False)):
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
            if s.get("loop"):
                # (a world spray's looping flipbook sheet: one picture for every spray alike, made by the converter;
                # kept in the cell cache too, for when this cell comes from it)
                gname = texture_out_name(tex)
                textures.generated[gname] = s["loop"][1]
                self.cell_generated.add(gname)
                png = self.cache_dir / "loops" / (gname + ".png")
                if not png.exists():
                    png.parent.mkdir(parents=True, exist_ok=True)
                    s["loop"][1].save(png)
            elif "rgba" not in s:
                textures.request_shape(s, key[1] & (FLAG_BLEND | FLAG_TEST))

        for key, ss in groups.items():
            for s in ss:
                use(s, key)
        for shapes in door_groups.values():
            for s in shapes:
                use(s, (s["tex"], s["flags"]))
        for sm in shared:
            for b in sm["batches"]:
                for sh in b["shapes"]:
                    use(sh, (sh["tex"], sh["flags"]))
                b["tex"] = tex_index.get(b["tex_path"], -1)
        for a in npc_models:
            for s in a["shapes"]:
                use(s, (s["tex"], s["flags"]))

        ranges = {}
        outdoor_lit = not env["interior"] or env.get("sky", False)
        batches = make_batches(groups, tex_index, lights, ambient, sun, sun_dir, ranges, split=outdoor_lit)
        for ri, rr in ranges.items():
            ref_out[ri]["ranges"] = rr
        door_meshes = []
        for ri, shapes in door_groups.items():
            g = {}
            for s in shapes:
                g.setdefault((s["tex"], s["flags"], s["alpha_ref"]), []).append(s)
            door_meshes.append({"ref": ri, "batches": make_batches(g, tex_index, lights, ambient, sun, sun_dir,
                                                                   split=outdoor_lit)})
        if outdoor_lit:
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
                            col_ranges, shared)
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
            tint, lamp = light_tint(place[:3, 3] + [0.0, 0.0, 64.0], lights, ambient, sun)
            act_out.append({"ref": a["ref"], "skeleton": skel_index[path], "place": place, "idle": a["idle"],
                            "lib": self.actor_library(a["id"], a["shapes"], sk),
                            "meshes": self.library_meshes[a["id"]],     # (for the memory estimate below)
                            "tint": tint, "lamp": lamp})
        write_actor_placements(base.with_suffix(".act"), act_out)
        nv = sum(len(b["verts"]) for b in batches)
        nt = sum(len(b["idx"]) for b in batches) // 3
        size = base.with_suffix(".cel").stat().st_size + base.with_suffix(".act").stat().st_size
        print(f"  {base.name}.cel: {len(batches)} batches, {nv} verts, {nt} tris, {len(door_meshes)} swing doors, "
              f"{len(sky)} sky batches, collision {len(collision[1])} tris, {len(tex_names)} textures, "
              f"{len(act_out)} animated actors; {size // 1024} KB on disk")
        if shared:
            print(f"  {base.name}.cel: {len(shared)} shared models, {sum(len(sm['places']) for sm in shared)} placements, "
                  f"{shared_bytes(shared) // 1024} KB (as batches {sum(sm['baked_bytes'] for sm in shared) // 1024} KB)")
        # Linear memory the 3DS needs for this cell's buffers (textures are added by the caller)
        buf = lambda bs: sum(len(b["verts"]) * 16 + len(b["idx"]) * 2 for b in bs)
        geometry = buf(batches) + buf(sky) + sum(buf(d["batches"]) for d in door_meshes) + shared_bytes(shared) + \
            sum(len(m["pos"]) * VERTEX.itemsize + len(np.asarray(m["tris"]).ravel()) * 2
                for a in act_out for m in a["meshes"])
        return {"refs": ref_out, "npcs": npc_recs, "file": base.name, "textures": tex_names,
                "geometry_bytes": geometry}
