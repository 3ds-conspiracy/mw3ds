"""Particle sprays simulated the way OpenMW runs a NIF's particle systems, turned into sprites the 3DS draws without
simulating them: particle records (each particle's birth, place, speed, life and size: the engine places it each
frame), flipbooks (a still spray rendered into a sheet of frames on one quad turned to the eye), and records shed into
the world by bolts and creatures' bones. level.py builds every spell visual's, world object's (WORLD_FX) and
creature's piece (build_pieces); internal/PFX-PROCESS.md has how each was matched against OpenMW.

    python tools/convert/particles.py --auto <static id> [--dry] [--out out/world]     one visual or world object
    python tools/convert/particles.py --creature <creature id> [--dry] [--out out/world]  one creature's sprays
    python tools/convert/particles.py --model e/magic_cast_ill.NIF --png build/particles/ill.png    a preview sheet

The rules are OpenMW's (components/nifosg/nifloader.cpp handleParticleEmitter, particle.cpp):
  emission   birth_rate a second when emit_flags & 1, else num_particles / (lifetime + lifetime_var / 2), while the
             controller's time is within emit start .. stop
  placement  at the emitter node, anywhere in a box of emitter_dims (centred)
  direction  in the emitter's frame: planar +- var round Z, then declination +- var from Z; speed +- var / 2
  lifetime   lifetime + var x random
  gravity    NiGravity wind: velocity += direction x force x 1.6 x dt (point: toward its position)
  grow/fade  size x age / grow early on, x (life - age) / fade near the end
  colour     NiParticleColorModifier keys over age / lifetime (else the controller's colour)
Particles stay where they were born in root space (a moving emitter leaves a trail).
  at start   the particles the model was saved with are alive at time 0, each with its age, life, velocity and size
             (handleParticleInitialState): a spray shows at once instead of building up
  quota      no more live particles than the particle data's count (OpenMW's ParticleSystem quota)
"""
import argparse
import io
import math
import random

import numpy as np
from PIL import Image

from meshes import last_prop, texture_path, walk_nif

SIM_RATE = 60          # simulation steps a second
GRAVITY_MAGIC = 1.6    # OpenMW's GravityAffector factor


def sprays(nif, vis_hidden=False):
    """Every particle system of a model: its controller, modifiers, the emitter's and the particles' node matrices
    (root space, at rest), texture, blending and material. vis_hidden: also those hidden at rest that a
    NiVisController shows (a creature's bursts)."""
    nodes, shapes = {}, []

    def visit(b, m, props):
        nodes[b["index"]] = m
        ctrl = nif.get(b.get("controller", -1))
        if ctrl and ctrl["type"] in ("NiParticleSystemController", "NiBSPArrayController"):
            shapes.append((b, m, props, ctrl))
    walk_nif(nif, visit, vis_hidden)
    out = []
    for b, m, props, ctrl in shapes:
        mods = {"gravity": [], "growfade": None, "color": None}
        mod = nif.get(ctrl.get("particle_modifier", -1))
        while mod:
            if mod["type"] == "NiGravity":
                mods["gravity"].append(mod)
            elif mod["type"] == "NiParticleGrowFade":
                mods["growfade"] = mod
            elif mod["type"] == "NiParticleColorModifier":
                d = nif.get(mod["color_data"])
                mods["color"] = d["keys"] if d and d["keys"] else None
            mod = nif.get(mod.get("next_modifier", -1))
        alpha = last_prop(props, "NiAlphaProperty")
        data = nif.get(b.get("data", -1))
        initial, quota = [], len(ctrl.get("particles", []))
        if data and data.get("vertices"):
            quota = data["quota"]
            verts = data["vertices"]
            for i, (vel, age, life, code) in enumerate(ctrl.get("particles", [])):
                if i >= data["active"]:
                    break
                if life <= 0 or code >= data["num_vertices"]:
                    continue
                size = ctrl["initial_size"] * (data["sizes"][code] if data["sizes"] else 1.0)
                initial.append((m[:3, :3] @ np.array(verts[3 * code:3 * code + 3]) + m[:3, 3],
                                m[:3, :3] @ np.array(vel), max(0.0, age), life, size))
        out.append({"initial": initial, "quota": quota, "ctrl": ctrl, "emitter": nodes.get(ctrl["emitter"]), "node": m, "mods": mods,
                    "texprop": last_prop(props, "NiTexturingProperty"),
                    "additive": bool(alpha and (alpha["flags"] >> 5) & 15 == 0),
                    # (source factor ONE: added whatever the particle's alpha, as magic_cast_frost's ribbon)
                    "alpha_one": bool(alpha and alpha["flags"] & 1 and (alpha["flags"] >> 1) & 15 == 0),
                    "mat": last_prop(props, "NiMaterialProperty"), "block": b["index"]})
    return [s for s in out if s["emitter"] is not None]


def color_at(keys, f, default):
    if not keys:
        return np.array(default, dtype=float)
    times = [k[0] for k in keys]
    cols = np.array([k[1] for k in keys], dtype=float)
    return np.array([np.interp(f, times, cols[:, j]) for j in range(4)])


def simulate(spray, duration, fps=20, seed=1, emitter_track=None):
    """Frames fps a second over duration: each a list of (pos[3] root space, half size, rgba[4]) for the live
    particles. emitter_track(t) -> 4x4 overrides the emitter's rest matrix (a bolt's emitter moving)."""
    c, mods = spray["ctrl"], spray["mods"]
    rng = random.Random(seed)
    rate = c["birth_rate"] if c.get("emit_flags", 0) & 1 else \
        c["num_particles"] / max(1e-6, c["lifetime"] + c.get("lifetime_var", 0.0) / 2)
    emissive = np.array(spray["mat"]["emissive"], dtype=float) if spray["mat"] else np.zeros(3)
    rot_node = spray["node"][:3, :3]
    gravity = []
    for g in mods["gravity"]:
        d = rot_node @ np.array(g["direction"], dtype=float)
        n = np.linalg.norm(d)
        gravity.append((g["force_type"], d / n if n > 1e-9 else d, spray["node"][:3, :3] @ np.array(g["position"]) +
                        spray["node"][:3, 3], g["force"]))
    gf = mods["growfade"]
    parts = [[p, v, age, life, size] for p, v, age, life, size in spray.get("initial", [])]   # [pos, vel, age, life, size]
    quota = spray.get("quota") or 10 ** 6
    acc = 0.0
    frames, dt = [], 1.0 / SIM_RATE
    steps = int(duration * SIM_RATE) + 1
    for i in range(steps):
        t = i * dt
        if c["emit_start"] <= t < c["emit_stop"]:
            acc += rate * dt
            em = emitter_track(t) if emitter_track else spray["emitter"]
            while acc >= 1.0:
                acc -= 1.0
                box = np.array([(rng.random() - 0.5) * d for d in c["emitter_dims"]])
                h = c["planar"] + c["planar_var"] * (2 * rng.random() - 1)
                v = c["declination"] + c["declination_var"] * (2 * rng.random() - 1)
                local = np.array([math.sin(v) * math.cos(h), math.sin(v) * math.sin(h), math.cos(v)])
                speed = c["speed"] - c["speed_var"] * 0.5 + c["speed_var"] * rng.random()
                life = max(1e-6, c["lifetime"] + c.get("lifetime_var", 0.0) * rng.random())
                pos = em[:3, :3] @ box + em[:3, 3]
                if len(parts) < quota:
                    parts.append([pos, em[:3, :3] @ local * speed, 0.0, life, c["initial_size"]])
        for p in parts:
            for kind, d, gpos, force in gravity:
                if kind == 0:
                    p[1] = p[1] + d * force * dt * GRAVITY_MAGIC
                else:
                    to = gpos - p[0]
                    n = np.linalg.norm(to)
                    if n > 1e-6:
                        p[1] = p[1] + to / n * force * dt * GRAVITY_MAGIC
            p[0] = p[0] + p[1] * dt
            p[2] += dt
        parts = [p for p in parts if p[2] < p[3]]
        if i * dt >= len(frames) / fps - 1e-6:          # (a frame every 1 / fps seconds, whatever the step)
            live = []
            for pos, vel, age, life, size in parts:
                if gf:
                    size = c["initial_size"]            # (GrowFadeAffector sizes every particle from the default)
                if gf and gf["grow"] > 0 and age < gf["grow"]:
                    size *= age / gf["grow"]
                if gf and gf["fade"] > 0 and life - age < gf["fade"]:
                    size *= (life - age) / gf["fade"]
                col = color_at(mods["color"], age / life, c["initial_color"])
                rgb = np.clip(col[:3] + emissive, 0, 1)        # lit at full, the material's glow on top
                live.append((pos.copy(), size, np.append(rgb, 1.0 if spray.get("alpha_one") else col[3])))
            frames.append(live)
    return frames


def particle_image(arch, nif, spray):
    path, _, _ = texture_path(arch, nif, spray["texprop"])
    if not path:
        return Image.new("RGBA", (16, 16), (255, 255, 255, 255))
    return Image.open(io.BytesIO(arch.read(path))).convert("RGBA")


def render_group(sets, additive, bounds, px_per_unit, axes=(0, 2)):
    """Several sprays of one blending drawn together: sets [(frames, sprite image)], frames the same length."""
    out = None
    for frames, sprite in sets:
        imgs = render_frames(frames, sprite, additive, bounds, px_per_unit, axes, raw=True)
        out = imgs if out is None else [a + b if additive else over(b, a) for a, b in zip(out, imgs)]
    return [finish(acc, additive) for acc in out]


def over(top, under):
    a = top[..., 3:4]
    res = np.empty_like(top)
    res[..., :3] = top[..., :3] * a + under[..., :3] * (1 - a)
    res[..., 3:4] = a + under[..., 3:4] * (1 - a)
    return res


def finish(acc, additive):
    acc = acc.copy()
    if additive:
        acc[..., 3] = 1.0
    else:
        a = np.maximum(acc[..., 3:4], 1e-6)
        acc[..., :3] = np.where(acc[..., 3:4] > 1e-6, acc[..., :3] / a, 0)
    return Image.fromarray((np.clip(acc, 0, 1) * 255).astype(np.uint8), "RGBA")


def render_frames(frames, sprite, additive, bounds, px_per_unit, axes=(0, 2), raw=False):
    """Each frame drawn as seen from the side (orthographic, axes: which root axes are image x and up), sprites the
    particle texture tinted by the particle's colour. Additive: summed colour on black, alpha 255 (drawn added);
    else alpha-composited over transparent. bounds: (min_x, min_up, max_x, max_up) in root units."""
    x0, y0, x1, y1 = bounds
    w = max(8, int(round((x1 - x0) * px_per_unit)))
    h = max(8, int(round((y1 - y0) * px_per_unit)))
    base = np.asarray(sprite, dtype=float) / 255.0
    out = []
    for live in frames:
        acc = np.zeros((h, w, 4))
        for pos, size, rgba in sorted(live, key=lambda p: p[0][1]):     # far (larger Y) first is not needed when added
            r = max(1, int(round(size * SIZE_X * px_per_unit)))
            spr = np.asarray(Image.fromarray((base * 255).astype(np.uint8)).resize((2 * r, 2 * r), Image.BILINEAR),
                             dtype=float) / 255.0
            tint = spr * rgba
            cx = (pos[axes[0]] - x0) * px_per_unit
            cy = (y1 - pos[axes[1]]) * px_per_unit
            xa, ya = int(round(cx)) - r, int(round(cy)) - r
            sx0, sy0 = max(0, -xa), max(0, -ya)
            sx1, sy1 = min(2 * r, w - xa), min(2 * r, h - ya)
            if sx0 >= sx1 or sy0 >= sy1:
                continue
            dst = acc[ya + sy0:ya + sy1, xa + sx0:xa + sx1]
            src = tint[sy0:sy1, sx0:sx1]
            if additive:
                dst[..., :3] += src[..., :3] * src[..., 3:4]
            else:
                a = src[..., 3:4]
                dst[..., :3] = src[..., :3] * a + dst[..., :3] * (1 - a)
                dst[..., 3:4] = a + dst[..., 3:4] * (1 - a)
        out.append(acc if raw else finish(acc, additive))
    return out


def frames_bounds(frame_sets, axes=(0, 2), pad=1.0):
    lo, hi = np.array([1e9, 1e9]), np.array([-1e9, -1e9])
    for frames in frame_sets:
        for live in frames:
            for pos, size, _ in live:
                p = np.array([pos[axes[0]], pos[axes[1]]])
                lo, hi = np.minimum(lo, p - size * pad), np.maximum(hi, p + size * pad)
    return (lo[0], lo[1], hi[0], hi[1])


def atlas(images, cols):
    w, h = images[0].size
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGBA", (w * cols, h * rows), (0, 0, 0, 0))
    for i, im in enumerate(images):
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    return sheet, rows


import os
# tuning (prototype rounds against OpenMW): sprite size x, supersampling, brightness x
SIZE_X = float(os.environ.get("PFX_SIZE", "1.0"))   # (drawn radius per unit of particle size: OpenMW draws size as the radius)
SUPER = int(os.environ.get("PFX_SS", "1"))
GAIN = float(os.environ.get("PFX_GAIN", "1.0"))


def flipbook_meshes(arch, nif, ss, sims, name, fps, cell=(64, 128), cols=8, turning=None):
    """One billboard quad per place sprays come from, standing at it (the places stay right from any side); the sprays
    from one place drawn together; places alike (one spray each, the same controller settings, picture and blending,
    emitter not turning) share one sheet of frames. turning: {spray index: True} for emitters that turn in place."""
    turning = turning or {}
    import textures
    # sprays from one place (same picture and blending) are drawn together into one sheet: a quad per place
    stacks = {}
    for i, sp in enumerate(ss):
        place = tuple(np.round(sp["emitter"][:3, 3]).astype(int))
        stacks.setdefault((place, sp["additive"], id(sp["texprop"])), []).append(i)
    # stacks alike (one spray each, same controller settings, emitter not turning) share one sheet
    groups = {}
    for key, idx in stacks.items():
        if len(idx) == 1 and not turning.get(idx[0]):
            c = ss[idx[0]]["ctrl"]
            sig = (round(c["speed"], 2), round(c["lifetime"], 3), round(c["initial_size"], 2), round(c["birth_rate"], 2),
                   round(c["emit_start"], 3), round(c["emit_stop"], 3), key[1], key[2])
        else:
            sig = key
        groups.setdefault(sig, []).append(idx)
    n = len(sims[0])
    meshes = []
    for gi, (sig, places) in enumerate(sorted(groups.items(), key=lambda kv: kv[1][0][0])):
        idx = places[0]
        sp = ss[idx[0]]
        origin = sp["emitter"][:3, 3]
        local = [[(p - origin, size, rgba) for j in idx for p, size, rgba in sims[j][f]] for f in range(n)]
        x0, y0, x1, y1 = frames_bounds([local], pad=SIZE_X)
        half = max(abs(x0), abs(x1))
        x0, x1 = -half, half                                # centred on the emitter (it turns round it)
        want = cell[1] / cell[0]
        if (y1 - y0) / (x1 - x0) < want:
            y1 = y0 + (x1 - x0) * want
        else:
            half = (y1 - y0) / want / 2
            x0, x1 = -half, half
        ppu = cell[0] * SUPER / (x1 - x0)
        additive = sp["additive"]
        imgs = render_group([(local, particle_image(arch, nif, sp))], additive, (x0, y0, x1, y1), ppu)
        imgs = [im.resize(cell, Image.BOX) for im in imgs]
        if GAIN != 1.0:
            imgs = [Image.fromarray(np.clip(np.asarray(im, dtype=float) * [GAIN, GAIN, GAIN, 1], 0, 255).astype(np.uint8), "RGBA")
                    for im in imgs]
        sheet, rows = atlas(imgs, cols)
        side = 1 << max(sheet.size[0] - 1, sheet.size[1] - 1).bit_length()
        full = Image.new("RGBA", (side, side), (0, 0, 0, 0 if not additive else 255))
        full.paste(sheet, (0, 0))
        tex = f"{name}_{gi}.t3x"
        if TEXTURES is not None:
            TEXTURES.made.add(tex)                 # (the level's texture pass keeps it)
        out = textures.tex_path(tex)
        out.parent.mkdir(parents=True, exist_ok=True)
        textures.encode_one(textures.tex3ds_path(), full, out, side, True)
        du, dv = cell[0] / side, cell[1] / side
        for i in (pl[0] for pl in places):
            e = ss[i]["emitter"][:3, 3]
            pos = np.array([[x0, y0, 0], [x1, y0, 0], [x1, y1, 0], [x0, y1, 0]], dtype=float)
            uv = np.array([[0, 1 - dv], [du, 1 - dv], [du, 1], [0, 1]])
            place = np.hstack([np.identity(3), e.reshape(3, 1)])
            meshes.append({"tex": tex, "flags": (1 | 4 | (16 if additive else 0)), "alpha_ref": 0, "kind": 0,
                           "bone": "", "pos": pos, "uv": uv, "tris": np.array([0, 1, 2, 0, 2, 3]),
                           "colors": np.full((4, 4), 255, dtype=np.uint8), "track": [place], "billboard": True,
                           "flip": (n, side // cell[0], side // cell[1], float(fps))})
    return meshes


def path_meshes(arch, nif, ss, sims, fps):
    """One mesh per blending: every particle two crossed quads, their corners baked frame by frame."""
    import textures
    groups = {}
    for sp, f in zip(ss, sims):
        groups.setdefault(sp["additive"], []).append((sp, f))
    meshes = []
    n = len(sims[0])
    for additive, items in sorted(groups.items()):
        cap = max(sum(len(f[k]) for _, f in items) for k in range(n))
        sp0 = items[0][0]
        path, _, _ = texture_path(arch, nif, sp0["texprop"])
        tex = textures.texture_out_name(path) if path else None
        frames = []
        for k in range(n):
            live = [p for _, f in items for p in f[k]]
            pos = np.zeros((cap * 8, 3))
            col = np.zeros((cap * 8, 4), dtype=np.uint8)
            for i, (p, size, rgba) in enumerate(live):
                h = size * 0.5
                quad = [(-h, 0, -h), (h, 0, -h), (h, 0, h), (-h, 0, h), (0, -h, -h), (0, h, -h), (0, h, h), (0, -h, h)]
                pos[i * 8:i * 8 + 8] = p + np.array(quad)
                col[i * 8:i * 8 + 8] = (np.clip(rgba, 0, 1) * 255).astype(np.uint8)
            frames.append((pos, col))
        tris = np.concatenate([np.array([0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]) + i * 8 for i in range(cap)])
        uv = np.tile(np.array([[0, 0], [1, 0], [1, 1], [0, 1]] * 2, dtype=float), (cap, 1))
        meshes.append({"tex": tex, "flags": (1 | 4 | (16 if additive else 0)), "alpha_ref": 0, "kind": 0, "bone": "",
                       "pos": frames[0][0], "uv": uv, "tris": tris, "colors": frames[0][1],
                       "path": (float(fps), frames)})
    return meshes


def prototype(model, static_id, out_dir, fps=20, duration=1.25):
    """The flipbook and baked-path versions of one spell visual, written as pieces '<piece>_flip' / '<piece>_path'
    into <out>/fp and listed in game_firstperson.json as '<static>#flip' / '#path' (picked at run time with the
    MW3DS_PFX setting, native test build)."""
    import json, struct, zlib
    from pathlib import Path
    from firstperson import write_piece
    from mwfiles import Archives
    from nif import Nif
    arch = Archives()
    nif = Nif(arch.read("meshes/" + model))
    ss = sprays(nif)
    # it plays for its longest controller's time, then goes all at once (OpenMW's FindMaxControllerLengthVisitor)
    stops = [b["stop_time"] for b in nif.blocks if "stop_time" in b]
    if stops and max(stops) > 0:
        duration = float(max(stops))
    sims = [simulate(sp, duration, fps) for sp in ss]
    out = Path(out_dir)
    base = static_id.replace("vfx_", "pfx_")
    pieces = {"flip": flipbook_meshes(arch, nif, ss, sims, base, fps), "path": path_meshes(arch, nif, ss, sims, fps)}
    for kind, meshes in pieces.items():
        write_piece(out / "fp" / f"{base}_{kind}.fpm", meshes)
    gp = out / "game_firstperson.json"
    raw = gp.read_bytes()
    data = zlib.decompress(raw[8:]) if raw[:4] == b"MWZ1" else raw
    g = json.loads(data)
    fp = g["firstperson"]
    # each magic effect's particle picture, its colour where it shows (alpha-weighted): a bolt's streak is drawn in it
    from mwfiles import read_game
    rgb = {}
    for r in read_game({"MGEF"}):
        t = (r.zstr("PTEX") or "").lower()
        for p in ("textures/" + t.rsplit(".", 1)[0] + ".dds", "textures/" + t):
            if t and arch.exists(p):
                a = np.asarray(Image.open(io.BytesIO(arch.read(p))).convert("RGBA"), dtype=float) / 255
                w = a[..., 3:4]
                c = (a[..., :3] * w).sum((0, 1)) / max(float(w.sum()), 1e-6)
                rgb[str(struct.unpack("<i", r.get("INDX"))[0])] = int(c[0] * 255) << 16 | int(c[1] * 255) << 8 | int(c[2] * 255)
                break
    fp["ptexrgb"] = rgb
    for kind in pieces:
        fp["vfx"][f"{static_id}#{kind}"] = f"{base}_{kind}"
        fp.setdefault("vfxlen", {})[f"{base}_{kind}"] = duration
    data = json.dumps(g, separators=(",", ":")).encode()
    gp.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 9))
    print(f"{static_id}: {len(ss)} sprays, {len(sims[0])} frames; pieces {base}_flip, {base}_path")


def emit_records(spray, duration, emitter_at=None, seed=1):
    """Every particle a spray emits over duration, once: (born, pos[3], vel[3], life, size) in root space; its emitter
    where its keyframes have it when the particle leaves (emitter_at(t) -> 4x4), else at rest."""
    c = spray["ctrl"]
    rng = random.Random(seed)
    rate = c["birth_rate"] if c.get("emit_flags", 0) & 1 else         c["num_particles"] / max(1e-6, c["lifetime"] + c.get("lifetime_var", 0.0) / 2)
    gf = spray["mods"]["growfade"]
    # (a saved particle is where it is now, at its age: its record starts where it was born; sprite_meshes takes
    # the pull off too)
    out = [[-age, *(pos - vel * age), *vel, life, c["initial_size"] if gf else size]
           for pos, vel, age, life, size in spray.get("initial", [])]
    quota = spray.get("quota") or 10 ** 6
    acc, dt = 0.0, 1.0 / SIM_RATE
    for i in range(int(duration * SIM_RATE) + 1):
        t = i * dt
        if not (c["emit_start"] <= t < c["emit_stop"]):
            continue
        acc += rate * dt
        em = emitter_at(t) if emitter_at else spray["emitter"]
        while acc >= 1.0:
            acc -= 1.0
            box = np.array([(rng.random() - 0.5) * d for d in c["emitter_dims"]])
            h = c["planar"] + c["planar_var"] * (2 * rng.random() - 1)
            v = c["declination"] + c["declination_var"] * (2 * rng.random() - 1)
            local = np.array([math.sin(v) * math.cos(h), math.sin(v) * math.sin(h), math.cos(v)])
            speed = c["speed"] - c["speed_var"] * 0.5 + c["speed_var"] * rng.random()
            life = max(1e-6, c["lifetime"] + c.get("lifetime_var", 0.0) * rng.random())
            pos = em[:3, :3] @ box + em[:3, 3]
            vel = em[:3, :3] @ local * speed
            if sum(1 for r in out if r[0] <= t < r[0] + r[7]) < quota:
                out.append([t, *pos, *vel, life, c["initial_size"]])
    return out


def sprite_meshes(arch, nif, ss, duration, still=False, light=None):
    """One mesh per spray drawn as sprites from its particle records (the engine works out where each is). still:
    the emitters' keyframes left out (a creature's records are in its bone's own frame)."""
    import textures
    from firstperson import vfx_tracks, VFX_FPS
    tracks = {} if still else vfx_tracks(nif)[0]
    meshes = []
    for i, sp in enumerate(ss):
        c, mods = sp["ctrl"], sp["mods"]
        e = c["emitter"]
        track = tracks.get(e)
        def at(t, tr=track):
            # the emitter's place between two samples of its path, blended (each particle where it really is)
            f = min(max(t * VFX_FPS, 0.0), len(tr) - 1)
            a = int(f)
            b = min(a + 1, len(tr) - 1)
            return tr[a] + (tr[b] - tr[a]) * (f - a)
        if not track:
            at = None
        recs = emit_records(sp, duration, at, seed=i + 1)
        accel = np.zeros(3)
        for g in mods["gravity"]:
            if g["force_type"] == 0:
                d = sp["node"][:3, :3] @ np.array(g["direction"], dtype=float)
                if os.environ.get("PFX_WIND_T"):
                    d = sp["node"][:3, :3].T @ np.array(g["direction"], dtype=float)
                n = np.linalg.norm(d)
                if n > 1e-9:
                    accel += d / n * g["force"] * GRAVITY_MAGIC
        for r in recs:
            if r[0] < 0:
                r[1:4] = list(np.array(r[1:4]) - 0.5 * accel * r[0] * r[0])
        emissive = np.array(sp["mat"]["emissive"], dtype=float) if sp["mat"] else np.zeros(3)
        keys = mods["color"] or [(0.0, list(c["initial_color"]))]
        # (the particle's colour lit: OpenMW lights it with the scene's light, ambient and diffuse; LIGHT stands for a
        # day outdoors; the material's glow on top)
        light = float(os.environ.get("PFX_LIGHT", "1.0")) if light is None else light
        keys = [(float(t), [int(round(v * 255)) for v in np.append(np.clip(np.array(col[:3]) * light + emissive, 0, 1),
                                                                      1.0 if sp.get("alpha_one") else np.clip(col[3], 0, 1))])
                for t, col in keys][:8]
        gf = mods["growfade"]
        path, _, _ = texture_path(arch, nif, sp["texprop"])
        if path and TEXTURES is not None:
            TEXTURES.request(path, 64.0, True)
        # (a spray drawn with the model root's first texture takes the magic effect's own particle texture, as its
        # shapes do: the default bolt's sparks are a Disintegrate Armor's fire; OpenMW's overrideFirstRootTexture)
        root = nif.blocks[0] if nif.blocks else {}
        root_tex = next((nif.get(q) for q in root.get("properties") or [] if nif.get(q) and
                         nif.get(q)["type"] == "NiTexturingProperty"), None)
        meshes.append({"fx": root_tex is not None and sp["texprop"] is root_tex,
                       "tex": textures.texture_out_name(path) if path else None,
                       "flags": (1 | 4 | 8 | (16 if sp["additive"] else 0)), "alpha_ref": 0, "kind": 0, "bone": "",
                       "pos": np.zeros((4, 3)), "uv": np.zeros((4, 2)), "tris": np.array([0, 1, 2]),
                       "colors": np.zeros((4, 4), dtype=np.uint8),
                       "sprites": {"accel": list(accel), "grow": gf["grow"] if gf else 0.0,
                                   "fade": gf["fade"] if gf else 0.0, "keys": keys,
                                   "records": recs}})
    return meshes


def prototype_sprites(model, static_id, out_dir):
    """A spell visual with its particle sprays as sprites from particle records and its other shapes as they are:
    piece '<piece>_spr', listed as '<static>#spr' (MW3DS_PFX=spr)."""
    import json, struct, zlib
    from pathlib import Path
    import textures
    from firstperson import FirstPerson, write_piece
    from mwfiles import Archives, load_db
    from nif import Nif
    arch = Archives()
    db = load_db()
    nif = Nif(arch.read("meshes/" + model))
    stops = [b["stop_time"] for b in nif.blocks if "stop_time" in b]
    duration = float(max(stops)) if stops else 1.0
    fp = FirstPerson(db, arch, textures.TextureSet(arch), Path(out_dir) / "fp")
    name = fp.vfx_model(model)
    shapes = [m for m in fp.pieces[name] if not m.get("particles")] if name else []
    meshes = shapes + sprite_meshes(arch, nif, sprays(nif), duration)
    base = static_id.replace("vfx_", "pfx_") + "_spr"
    write_piece(Path(out_dir) / "fp" / f"{base}.fpm", meshes)
    gp = Path(out_dir) / "game_firstperson.json"
    raw = gp.read_bytes()
    g = json.loads(zlib.decompress(raw[8:]) if raw[:4] == b"MWZ1" else raw)
    g["firstperson"]["vfx"][f"{static_id}#spr"] = base
    g["firstperson"].setdefault("vfxlen", {})[base] = duration
    data = json.dumps(g, separators=(",", ":")).encode()
    gp.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 9))
    n = sum(len(m["sprites"]["records"]) for m in meshes if m.get("sprites"))
    print(f"{static_id}: {len(shapes)} shapes kept, {n} particles in {len(meshes) - len(shapes)} sprays; piece {base}")


BUDGET = int(os.environ.get("PFX_BUDGET", "100"))     # live sprites an effect may have at its peak
FLIP_CELL = (32, 64)                                     # a flipbook frame (8 x 4 of them: 256 x 256, 64 KB)
FLIP_MAX = 32
RECORDS_MAX = 8192                                       # bytes of particle records an effect may carry
# Thinning shares tuned against OpenMW side by side (effect: {spray block: keep 1 in k}): a spray that builds a solid
# band by overlap (magic_cast_frost's ribbon) loses it when thinned as hard as loose sparks
THIN = {"vfx_frostcast": {23: 2, 12: 5}, "vfx_hands": {10: 3, 19: 3}}
# (the casting hands' glow, two on every caster at once: a third of its sparks)
# Methods chosen against OpenMW side by side where the rule picks worse: the soul trap's sparkles fly out wide and a
# flipbook frame spread over them shrank them into a flat glow; the lava river steam sits on the ground, where the
# flipbook quad was cut off by it in a straight line; the Mysticism hit's sprays all take the effect's own particle
# texture (Dispel's white sparkles, Detect Animal's ...), which a flipbook baked in the model's texture can't
METHOD = {"vfx_soul_trap": "records", "steam_lavariver": "records", "in_strong_platform": "flip",
          "dagoth ur death sparkles": "records", "vfx_hands": "records", "vfx_mysticismhit": "records",
          # (Tribunal: Sotha Sil's sparks, a few bright dots rising: a flipbook frame blurred them into a glowing column)
          "light_sotha_sparks": "records", "light_sotha_sparks_hi": "records"}


def ensure_textures(arch, ts, meshes):
    """Encodes the pieces' textures that out/tex doesn't have yet (a world object's or a spray's picture that no cell
    asks for any more, now the cells leave those objects out: without it they drew as white squares)."""
    import io, json
    import textures
    from PIL import Image
    want = dict(ts.req)
    cache = json.loads(textures.CACHE.read_text()) if textures.CACHE.exists() else {}
    names = {m["tex"] for m in meshes if m.get("tex")}
    n = 0
    for path, r in want.items():
        name = textures.texture_out_name(path)
        if name not in names or textures.tex_path(name).exists():
            continue
        img = Image.open(io.BytesIO(arch.read(path)))
        img.load()
        limit = ts.limit(max(r["px"], 64.0))
        entry = textures.encode_one(textures.tex3ds_path(), img.convert("RGBA"), textures.tex_path(name), limit, True)
        entry.update({"src": path, "limit": limit, "want_alpha": True, "overlay": ""})
        cache[name] = entry
        n += 1
    if n:
        textures.CACHE.write_text(json.dumps(cache, indent=0, sort_keys=True))
    return n


# A world spray's looping flipbook (a candle's flame, a chimney's smoke, placed on its cell's crossed quads):
# frames over one second and a third, a sheet of 4 x 4 cells; the cells' sprays share it, the frame from world time
LOOP_FRAMES, LOOP_COLS, LOOP_FPS = 16, 4, 12.0
_LOOPS = {}


def loop_flipbook(arch, nif, block):
    """The flipbook of the spray on particle shape `block` when its emitter stands still: (texture name, PIL sheet,
    (frames, cols, rows, fps), bounds (x0, up0, x1, up1) round the emitter, root units), or None. The spray is
    simulated as OpenMW runs it until it is steady, then a loop of frames drawn from the side, each particle in its
    colour at its age (the colour keys over its life: a flame's tips redden). The light falls on the quads, as on the
    cell's other shapes (meshes.particle_shape: a candle's flame is as bright as the room round it, OpenMW lights
    particles), the material's glow on top. Shared by every spray alike."""
    import hashlib
    from firstperson import vfx_tracks
    sp = next((x for x in sprays(nif, vis_hidden=True) if x["block"] == block), None)
    if sp is None:
        return None
    tracks, _, _ = vfx_tracks(nif)
    if sp["ctrl"]["emitter"] in tracks:
        return None                                   # (a moving emitter: not a still loop)
    c = sp["ctrl"]
    path, _, _ = texture_path(arch, nif, sp["texprop"])
    key = repr((path, round(c["speed"], 2), round(c["speed_var"], 2), round(c["lifetime"], 3), round(c["initial_size"], 2),
                round(c["birth_rate"], 2), c.get("emit_flags"), c.get("num_particles"), round(c["declination"], 3),
                round(c["declination_var"], 3), tuple(round(v, 1) for v in c["emitter_dims"]), sp["additive"],
                [(g["force_type"], round(g["force"], 2), tuple(round(v, 3) for v in g["direction"])) for g in sp["mods"]["gravity"]],
                sp["mods"]["growfade"] and (sp["mods"]["growfade"]["grow"], sp["mods"]["growfade"]["fade"]),
                tuple(round(v, 3) for v in (sp["node"][:3, :3] @ np.array([0, 0, 1.0]))),
                [(round(t, 3), [round(v, 3) for v in col]) for t, col in sp["mods"]["color"] or []],
                [round(v, 3) for v in c["initial_color"]], sp["mat"] and [round(v, 3) for v in sp["mat"]["emissive"]],
                sp.get("alpha_one")))
    if key in _LOOPS:
        return _LOOPS[key]
    warm = c["lifetime"] + c.get("lifetime_var", 0.0)
    span = LOOP_FRAMES / LOOP_FPS
    total = warm + span
    local = dict(sp, initial=[], mat=None)       # (no glow: the cell lights the quads, the glow added there)
    local["ctrl"] = dict(c, emit_start=0.0, emit_stop=total + 1.0)
    frames = simulate(local, total, LOOP_FPS)
    origin = sp["emitter"][:3, 3]
    loop = [[(p - origin, size, rgba) for p, size, rgba in live] for live in frames[-LOOP_FRAMES:]]
    if not any(loop):
        _LOOPS[key] = None
        return None
    x0, y0, x1, y1 = frames_bounds([loop], pad=SIZE_X)
    half = max(abs(x0), abs(x1), 1.0)
    x0, x1 = -half, half
    cw, ch = (32, 64) if (y1 - y0) >= (x1 - x0) else (64, 32)
    want = ch / cw
    if (y1 - y0) / (x1 - x0) < want:
        mid = (y0 + y1) / 2
        y0, y1 = mid - (x1 - x0) * want / 2, mid + (x1 - x0) * want / 2
    else:
        half = (y1 - y0) / want / 2
        x0, x1 = -half, half
    imgs = render_group([(loop, particle_image(arch, nif, sp))], sp["additive"], (x0, y0, x1, y1), cw / (x1 - x0))
    imgs = [im.resize((cw, ch), Image.BOX) for im in imgs]
    rows = -(-LOOP_FRAMES // LOOP_COLS)
    sheet = Image.new("RGBA", (cw * LOOP_COLS, ch * rows), (0, 0, 0, 0 if not sp["additive"] else 255))
    for i, im in enumerate(imgs):
        sheet.paste(im, ((i % LOOP_COLS) * cw, (i // LOOP_COLS) * ch))
    # (named after its particle picture too: tests find a spray's batches by it, texlow)
    import os, re
    stem = re.sub(r"[^a-z0-9_]", "_", os.path.splitext(os.path.basename((path or "").replace("\\", "/")))[0].lower())
    name = "loop_" + (stem + "_" if stem else "") + hashlib.sha1(key.encode()).hexdigest()[:12] + ".t3x"
    _LOOPS[key] = (name, sheet, (LOOP_FRAMES, LOOP_COLS, rows, LOOP_FPS), (x0, y0, x1, y1))
    return _LOOPS[key]


def is_bolt(db, static_id):
    """A magic effect flies it as its bolt (MGEF BVFX)."""
    import struct
    from mwfiles import read_game
    global _BOLTS
    if _BOLTS is None:
        _BOLTS = {(r.zstr("BVFX") or "").lower() for r in read_game({"MGEF"})}
        _BOLTS.add("vfx_defaultbolt")
    return static_id.lower() in _BOLTS


_BOLTS = None
TEXTURES = None          # the TextureSet a piece's textures are asked of (auto / creature_auto), for ensure_textures


def peak_live(record_sets, duration, step=0.05):
    """The most records alive at once (each from its birth for its life), as a sweep over their births and ends."""
    ev = sorted([(r[0], 1) for recs in record_sets for r in recs] + [(r[0] + r[7], -1) for recs in record_sets for r in recs],
                key=lambda e: (e[0], e[1]))
    best = live = 0
    for _, d in ev:
        live += d
        best = max(best, live)
    return best


def make_piece(static_id, db, arch, fp, ts):
    """A spell visual or world object made drawable the best way spray by spray (internal/PFX-PROCESS.md): sprays whose
    emitter stands still for the effect's life as flipbooks (a quad per emitter), sprays whose emitter moves as particle
    records, within the sprite budget; its other shapes kept (a world object's ambient shapes stay in its cell). fp: the
    FirstPerson its shapes come from, ts: the TextureSet its textures are asked of. Returns {"base": piece name,
    "meshes", "duration", "report": [lines], "world": its world_entry or None}."""
    global TEXTURES
    from firstperson import vfx_tracks, VFX_FPS
    from nif import Nif
    TEXTURES = ts
    rec = db["objects"][static_id.lower()]
    model = rec.zstr("MODL")
    nif = Nif(arch.read("meshes/" + model))
    stops = [b["stop_time"] for b in nif.blocks if "stop_time" in b]
    duration = float(max(stops)) if stops else 1.0
    tracks, _, _ = vfx_tracks(nif)
    ss = sprays(nif)
    def wanders(sp):
        tr = tracks.get(sp["ctrl"]["emitter"])
        return bool(tr) and max(np.linalg.norm(m[:3, 3] - tr[0][:3, 3]) for m in tr) > 1.0
    def flat(sp):
        # a spray spread out level (a ring on the ground): a quad facing the eye shows it edge on, as a line
        pts = [p - sp["emitter"][:3, 3] for live in simulate(sp, duration, 10) for p, _, _ in live]
        if not pts:
            return False
        pts = np.array(pts)
        across = np.abs(pts[:, :2]).max()
        return across > 2 * sp["ctrl"]["initial_size"] and np.ptp(pts[:, 2]) < 0.35 * across
    # A bolt's sprays: particles it leaves behind as it flies (the engine sheds each record every emission cycle where
    # the bolt was when it left): all records, one cycle, none of the model's saved ones (they'd show at the hands)
    bolt = is_bolt(db, static_id)
    # (an emitter that only turns in place still suits a flipbook: simulated turning)
    still = [] if bolt else [sp for sp in ss if not wanders(sp) and not flat(sp)]
    moving = list(ss) if bolt else [sp for sp in ss if wanders(sp) or flat(sp)]
    # few enough particles for sprites all through: records for every spray (a few hundred bytes, right from any
    # side, sharp however far they spread; a flipbook frame stretched over a long fall shrinks them to a pixel)
    if not bolt and still and peak_live([emit_records(sp, duration) for sp in ss], duration) <= BUDGET:
        moving, still = ss, []
    if os.environ.get("PFX_FORCE") == "records" or METHOD.get(static_id.lower()) == "records":
        # (every spray as records, thinned: PFX_FORCE to try it, METHOD where the side by side chose it)
        moving, still = ss, []
    if os.environ.get("PFX_FORCE") == "flip" or METHOD.get(static_id.lower()) == "flip":
        # (every spray as a flipbook, moving emitters simulated on their path: trails too dense to thin, the
        # Strong platform's swirls of 480 particles each)
        moving, still = [], list(ss)
    played = static_id.lower() in animated_collision_objects(db, arch)
    scripted = static_id.lower() in scripted_group_objects(db)
    world = None if is_spell_visual(static_id) else world_entry(nif, "", played, scripted)
    if world and world["groups"]:
        from firstperson import piece_name
        fp.light = room_light(db, static_id)
        fp.pieces.pop(piece_name(model, "vfx"), None)     # (its shapes lit as the room, not as a held item)
    name = fp.vfx_model(model)
    fp.light = None
    shapes = [m for m in fp.pieces[name] if not m.get("particles")] if name else []
    if world and not world["groups"]:
        shapes = []            # (an ambient one's shapes stay in its cell, baked; only its sprays are the piece's)
    base = static_id.lower().replace("vfx_", "pfx_") + "_auto"
    meshes = list(shapes)
    report = [f"{static_id} ({model}): {duration:.2f} s, {len(shapes)} shapes kept"]
    if still:
        fps = min(20.0, (FLIP_MAX - 1) / duration)
        def track_at(tr):
            def at(t):
                f = min(max(t * VFX_FPS, 0.0), len(tr) - 1)
                a = int(f)
                b = min(a + 1, len(tr) - 1)
                return tr[a] + (tr[b] - tr[a]) * (f - a)
            return at
        turning = {i: True for i, sp in enumerate(still) if sp["ctrl"]["emitter"] in tracks}
        sims = [simulate(sp, duration, fps, seed=i + 1,
                         emitter_track=track_at(tracks[sp["ctrl"]["emitter"]]) if i in turning else None)
                for i, sp in enumerate(still)]
        flips = flipbook_meshes(arch, nif, still, sims, base, fps, cell=FLIP_CELL, cols=8, turning=turning)
        meshes += flips
        report.append(f"  flipbook: {len(still)} sprays still -> {len(flips)} quads, {len(sims[0])} frames at {fps:.1f}/s")
    if moving:
        spr = sprite_meshes(arch, nif, moving, duration)
        if bolt:
            for m in spr:
                m["sprites"]["records"] = [r for r in m["sprites"]["records"] if r[0] >= 0.0]
        sets = [m["sprites"]["records"] for m in spr]
        peak = peak_live(sets, duration)
        total = sum(len(r) for r in sets)
        # (live sprites, and the records' size; a bolt's live ones are the engine's to share out)
        k = max(1, 1 if bolt else -(-peak // BUDGET), -(-total * 36 // RECORDS_MAX))
        # (PFX_KEEP="<spray block>:<k>,...": a spray's own thinning, for trying unequal shares)
        own = dict(THIN.get(static_id.lower(), {}))
        own.update({int(a): int(b) for a, b in (x.split(":") for x in os.environ.get("PFX_KEEP", "").split(",") if x)})
        if k > 1 or own:
            for m, sp in zip(spr, moving):
                k = own.get(sp["block"], k)
                kept = m["sprites"]["records"][::k]
                for r in kept:
                    r[8] *= k ** 0.5                   # (each stands for k: as much area)
                m["sprites"]["records"] = kept
        peak2 = peak_live([m["sprites"]["records"] for m in spr], duration)
        n = sum(len(m["sprites"]["records"]) for m in spr)
        meshes += spr
        report.append(f"  records: {len(moving)} sprays moving or flat -> {n} particles ({n * 36 // 1024 + 1} KB), peak "
                      f"{peak} live" + (f", thinned 1 in {k} -> {peak2}" if k > 1 else ""))
    if world is not None:
        world = world_entry(nif, base, played, scripted)
        report.append(f"  world object: {world}")
    return {"base": base, "meshes": meshes, "duration": duration, "report": report, "world": world}


def auto(static_id, out_dir, dry=False):
    """make_piece on its own (trying an effect): the piece into out_dir/fp, listed in its game_firstperson.json."""
    from pathlib import Path
    import textures
    from firstperson import FirstPerson, write_piece
    from mwfiles import Archives, load_db
    arch, db = Archives(), load_db()
    ts = textures.TextureSet(arch)
    r = make_piece(static_id, db, arch, FirstPerson(db, arch, ts, Path(out_dir) / "fp"), ts)
    if not dry:
        write_piece(Path(out_dir) / "fp" / f"{r['base']}.fpm", r["meshes"])
        added = ensure_textures(arch, ts, r["meshes"])
        if added:
            r["report"].append(f"  {added} textures encoded")
        edit_firstperson(out_dir, lambda fpj: register(fpj, static_id, r))
    print(chr(10).join(r["report"]))


def register(fpj, static_id, r):
    """A piece listed in game firstperson: as the static's own visual, its length, and a world object's entry."""
    sid = static_id.lower()
    fpj["vfx"][sid] = r["base"]
    fpj.setdefault("vfxlen", {})[r["base"]] = r["duration"]
    if r["world"] is not None:
        fpj.setdefault("worldfx", {})[sid] = r["world"]


def edit_firstperson(out_dir, fn):
    """game_firstperson.json read, changed by fn(its "firstperson" dict), written back."""
    import json, struct, zlib
    from pathlib import Path
    gp = Path(out_dir) / "game_firstperson.json"
    raw = gp.read_bytes()
    g = json.loads(zlib.decompress(raw[8:]) if raw[:4] == b"MWZ1" else raw)
    fn(g["firstperson"])
    data = json.dumps(g, separators=(",", ":")).encode()
    gp.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 9))


def build_pieces(db, arch, ts, fp, fpj, log=print):
    """The level conversion's particle pieces (level.py, after FirstPerson.build): every spell visual, the world
    objects (WORLD_FX) and every creature with sprays, put in fp.pieces (FirstPerson.write writes them) and listed in
    fpj (game firstperson). The spell visuals' plain pieces they replace go."""
    replaced = set()
    statics = sorted(fpj["vfx"])
    anim = animated_collision_objects(db, arch)
    shared = {}       # model -> its piece (Sotha Sil's eight doors of one model: one piece, read once)
    for sid in statics + [w for w in WORLD_FX + sorted(anim) if w not in fpj["vfx"]]:
        if sid not in db["objects"] or not db["objects"][sid].zstr("MODL"):
            continue
        old = fpj["vfx"].get(sid)
        model = db["objects"][sid].zstr("MODL").lower()
        if sid in anim and model in shared:
            r = shared[model]
        else:
            r = make_piece(sid, db, arch, fp, ts)
            if sid in anim:
                shared[model] = r
        fp.pieces[r["base"]] = r["meshes"]
        register(fpj, sid, r)
        if old and old != r["base"]:
            replaced.add(old)
    still_used = set(fpj["vfx"].values())
    for name in replaced - still_used:
        fp.pieces.pop(name, None)
    n = 0
    for cid in creatures_with_sprays(db, arch):
        r = make_creature_piece(cid, db, arch, ts)
        if r:
            fp.pieces[r["base"]] = r["meshes"]
            fpj.setdefault("cpfx", {})[cid] = r["base"]
            n += 1
    log(f"  particle pieces: {len(statics)} visuals, {len(fpj.get('worldfx', {}))} world objects, {n} creatures")


def creatures_with_sprays(db, arch):
    """Creature ids whose model has a particle system (its sprays to shed from its bones), each model read once."""
    from nif import Nif
    has = {}
    out = []
    for oid, rec in sorted(db["objects"].items()):
        if rec.tag != "CREA" or not rec.zstr("MODL"):
            continue
        model = rec.zstr("MODL").lower()
        if model not in has:
            try:
                nif = Nif(arch.read("meshes/" + model))
                has[model] = any(b["type"] in ("NiParticleSystemController", "NiBSPArrayController")
                                 for b in nif.blocks)
            except Exception:
                has[model] = False
        if has[model]:
            out.append(oid)
    return out


# World objects drawn as their particle pieces where they are placed (internal/PARTICLES-AUDIT.md groups C, D, E): the
# cells leave out their old crossed quads (convert_cell), or the whole object when it is animated (death groups)
WORLD_FX = ["furn_moldcave_pool00", "furn_lavacave_pool00", "furn_bonecave_pool00", "furn_mudcave_pool00",
            "furn_pycave_pool00", "steam_bluegreen", "steam_lavariver", "ex_lavaspark01", "ex_lavaspark03",
            "blight cloud", "in_strong_platform", "dagoth ur death sparkles", "in_akulakhan00", "in_dagoth_bridge00",
            "akhul_steam",
            # Tribunal's dart trap projectile (PlaceItem by the Forgotten Sewer's mines): a magic visual that flies;
            # Sotha Sil's sparks (49 in the Chamber of Sohleh), shed as single sparks
            "trapprojectile", "light_sotha_sparks", "light_sotha_sparks_hi",
            # Bamz-Amschend's cave-in: its boulders fly apart in its Death1 when the satchel charge goes off
            "act_dwe_cavein00",
            # Sotha Sil's steam vents (the Imperfect's bursts once in its Idle2 as it wakes) and Mournhold's pavilion
            # fountain spray: as crossed quads they stood as solid white columns
            "act_sotha_fabsteam", "act_sotha_steam00", "act_sotha_steam10", "ex_mh_pav_water_spray",
            # Bloodmoon: the werewolf's change flash (were_morph: the scripts place it where a wilderness werewolf or
            # the Skaal changes, and it removes itself) and the spriggan's rise as it gets up again (spriggan_summon)
            "wildwerewolf", "bm_wereeffect2", "bm_wereeffect3", "sprigganup"]


def world_fx_kinds(db, arch):
    """{object id: True when animated (drawn whole as its piece), False when only its sprays are}."""
    from nif import Nif
    out = {}
    anim = animated_collision_objects(db, arch)
    for sid in WORLD_FX + sorted(anim):
        rec = db["objects"].get(sid)
        if rec is None or not rec.zstr("MODL"):
            continue
        out[sid] = bool(world_entry(Nif(arch.read("meshes/" + rec.zstr("MODL"))), "", sid in anim,
                                    sid in scripted_group_objects(db))["groups"])
    return out


_ANIMATED_COLLISION = {}


def animated_collision_objects(db, arch):
    """Activators a script plays a group on (their own script's PlayGroup / LoopGroup, or another's "id"->PlayGroup),
    other than a death group, whose model's collision is animated (OpenMW's animated collision shapes: nodes with a
    NiKeyframeController under the RootCollisionNode): Tribunal's Clockwork City doors, opened by their levers. Drawn
    as their pieces (world_entry keeps their groups) and their moving collision kept apart in the cell, so the engine
    can take it away once the door is open and put it back once shut (world_entry's "away" / "back")."""
    if id(db) in _ANIMATED_COLLISION:
        return _ANIMATED_COLLISION[id(db)]
    from meshes import collision_triangles
    from nif import Nif
    out, moving = set(), {}
    for oid in scripted_group_objects(db):
        model = db["objects"][oid].zstr("MODL").lower()
        if model not in moving:
            try:
                col = collision_triangles(Nif(arch.read("meshes/" + model)), parts=True)
                moving[model] = col is not None and col[2] < len(col[1])
            except Exception:
                moving[model] = False
        if moving[model]:
            out.add(oid)
    _ANIMATED_COLLISION[id(db)] = out
    return out


_SCRIPTED_GROUPS = {}


def scripted_group_objects(db):
    """Activators a script plays a group other than a death group on: their own script's PlayGroup / LoopGroup, or
    another's "id"->PlayGroup (the Imperfect's script wakes its steam vent with act_sotha_fabsteam->PlayGroup Idle2)."""
    if id(db) in _SCRIPTED_GROUPS:
        return _SCRIPTED_GROUPS[id(db)]
    import re
    named, own = set(), set()
    for name, text in db["SCPT"].items():
        for line in (text or "").replace("\r", "").split("\n"):
            line = line.split(";")[0]
            m = re.search(r'(?i)(?:"([^"\n]+)"|([\w\'-]+))\s*->\s*(?:playgroup|loopgroup)\s*,?\s*"?(\w+)', line)
            if m:
                if not m.group(3).lower().startswith("death"):
                    named.add((m.group(1) or m.group(2)).strip().lower())
                continue
            m = re.match(r'(?i)\s*(?:playgroup|loopgroup)\s*,?\s*"?(\w+)', line)
            if m and not m.group(1).lower().startswith("death"):
                own.add(name)
    out = {oid for oid, rec in db["objects"].items() if rec.tag == "ACTI" and rec.zstr("MODL")
           and (oid in named or (rec.zstr("SCRI") or "").lower() in own)}
    _SCRIPTED_GROUPS[id(db)] = out
    return out


def posed_local(nif, t):
    """local(block index, block) for meshes.collision_triangles: each node's transform at t seconds into the model, its
    keyframes sampled (as vfx_tracks does for the drawn shapes)."""
    from firstperson import controller_time, xyz_matrix
    from meshes import nif_local
    from npc import quat_matrix, sample
    keys = {}
    for i, b in enumerate(nif.blocks):
        c = nif.get(b.get("controller", -1))
        while c:
            if c["type"] == "NiKeyframeController" and nif.get(c.get("data", -1)):
                keys[i] = (c, nif.get(c["data"]))
                break
            c = nif.get(c.get("next_controller", -1))

    def local(i, b):
        if i not in keys or "rotation" not in b:
            return nif_local(b)
        c, kd = keys[i]
        tt = controller_time(c, t)
        rot = np.array(b["rotation"]).reshape(3, 3)
        trans, scale = np.array(b["translation"]), b["scale"]
        if kd["rotations"]:
            rot = quat_matrix(sample(kd["rotations"], tt, quat=True))
        elif kd["xyz_rotations"]:
            rot = xyz_matrix(*[sample(k, tt)[0] if k else 0.0 for k in kd["xyz_rotations"]])
        if kd["translations"]:
            trans = sample(kd["translations"], tt)
        if kd["scales"]:
            scale = sample(kd["scales"], tt)[0]
        m = np.identity(4)
        m[:3, :3] = rot * scale
        m[:3, 3] = trans
        return m
    return local


def collision_moves(nif, groups):
    """({group: seconds}, {group: seconds}) for a model whose collision is animated: a group that leaves it away from
    where it rests ("away": a door's opening) takes it away that long after the group starts, once every part that
    moves is more than half its way there; a group that brings it back to rest ("back": the closing) puts it back once
    every moved part is less than half its way from rest. (The cell's collision is the rest pose's: the engine switches
    the moving part off and on, not following it, OpenMW's animated collision cut to its two ends.)"""
    from meshes import collision_triangles
    col = collision_triangles(nif, parts=True)
    if col is None or col[2] >= len(col[1]):
        return {}, {}
    pick = np.unique(col[1][col[2]:])
    rest = col[0][pick]

    def at(t):
        return collision_triangles(nif, posed_local(nif, t), parts=True)[0][pick]
    away, back = {}, {}
    for g, (start, stop) in groups.items():
        if stop - start < 0.05:
            continue
        times = [start + k / 15.0 for k in range(int((stop - start) * 15.0) + 1)] + [stop]
        dist = [np.linalg.norm(at(t) - rest, axis=1) for t in times]
        if dist[-1].max() > 8.0:
            moved, d = dist[-1] > 8.0, dist[-1]
            ok = [bool(np.all(x[moved] >= 0.5 * d[moved])) for x in dist]
            table = away
        elif dist[0].max() > 8.0:
            moved, d = dist[0] > 8.0, dist[0]
            ok = [bool(np.all(x[moved] <= 0.5 * d[moved])) for x in dist]
            table = back
        else:
            continue
        k = len(ok)
        while k > 0 and ok[k - 1]:
            k -= 1
        table[g] = round(times[min(k, len(times) - 1)] - start, 3)
    return away, back


def room_light(db, static_id):
    """(ambient, sun, sun direction) of the interior the object is placed in (CELL AMBI), as the cell's baking."""
    import struct as st
    from mwfiles import cell_refs
    from convert_cell import INTERIOR_SUN_DIR, color3
    cells = db["CELL"].values() if isinstance(db["CELL"], dict) else db["CELL"]
    for c in cells:
        ambi = c.get("AMBI")
        if ambi and any((r.get("id") or "").lower() == static_id.lower() for r in cell_refs(c)):
            return color3(ambi, 0), color3(ambi, 4), INTERIOR_SUN_DIR
    return None


def is_spell_visual(static_id):
    """A magic effect plays it (MGEF CVFX / BVFX / HVFX / AVFX), or it is one of the vfx_ statics."""
    from mwfiles import read_game
    global _SPELL_VFX
    if _SPELL_VFX is None:
        _SPELL_VFX = {(r.zstr(t) or "").lower() for r in read_game({"MGEF"})
                      for t in ("CVFX", "BVFX", "HVFX", "AVFX")}
    sid = static_id.lower()
    return sid in _SPELL_VFX or sid.startswith("vfx_")


_SPELL_VFX = None


def world_entry(nif, base, played=False, scripted=False):
    """How a world object's piece plays where it is placed: its animation groups from the text keys ("Death1":
    [start, stop]: an activator a script plays, held at rest until then), else over and over when its controllers
    go round (a steaming pool, Dagoth Ur's death sparkles from when they are enabled at his death). played: one of
    animated_collision_objects (a Clockwork City door): its groups kept, with when its collision goes and comes back
    (collision_moves). scripted: a script plays a group on it (scripted_group_objects): its groups kept, its sprays on
    the group's clock as OpenMW runs an animated object's particle controllers (Animation::addAnimSource), so the
    Imperfect's steam vent bursts once in the Idle2 its script plays and stands still at rest."""
    groups = {}
    for b in nif.blocks:
        if b["type"] != "NiTextKeyExtraData":
            continue
        for t, text in b.get("keys", []):
            for line in text.replace(chr(13), "").split(chr(10)):
                if ":" in line:
                    name, what = (x.strip().lower() for x in line.split(":", 1))
                    if what in ("start", "stop"):
                        groups.setdefault(name, [0.0, 0.0])[0 if what == "start" else 1] = round(float(t), 3)
    # (only a model with a death group waits for a script, the collapse of the Akulakhan chamber; an ambient one with an
    # idle group of its own, the lava river's steam and sparks, goes round as the rest)
    away, back = collision_moves(nif, groups) if played and groups else ({}, {})
    if not any(g.startswith("death") for g in groups) and not away and not back and not scripted:
        groups = {}
    loop = not groups
    out = {"piece": base, "loop": loop, "groups": groups}
    if away or back:
        out["away"], out["back"] = away, back
    return out


def bsp_targets(nif, c, recs, seed=1):
    """A spray's records split by the node each leaves from: [(node name, records)], positions in that node's frame.
    A NiBSPArrayController (OpenMW's handleParticleEmitter / Emitter::emitParticles) emits each particle from a node
    picked at random among its emitter and every node under it: with flag 8 at a random point on the bone from that
    node's parent to it; with flag 16 from a random vertex of the first shape under the emitter instead. Others from
    the emitter itself."""
    from meshes import nif_local
    em = nif.get(c["emitter"])
    name = (em.get("name") or "").lower()
    if c["type"] != "NiBSPArrayController":
        return [(name, recs)]
    rng = random.Random(seed)
    flags = c.get("ctrl_flags", 0)
    if flags & 16:
        # the first shape under the emitter: its vertices in the emitter's frame
        def first_shape(i, m):
            b = nif.get(i)
            if b is None:
                return None
            ml = m @ nif_local(b)
            if b["type"] in ("NiTriShape", "NiTriStrips"):
                d = nif.get(b.get("data", -1))
                if d and d.get("vertices"):
                    v = np.array(d["vertices"]).reshape(-1, 3)
                    return v @ ml[:3, :3].T + ml[:3, 3]
            for ch in b.get("children", []) or []:
                found = first_shape(ch, ml)
                if found is not None:
                    return found
            return None
        verts = None
        for ch in em.get("children", []) or []:
            verts = first_shape(ch, np.identity(4))
            if verts is not None:
                break
        if verts is None or not len(verts):
            return [(name, recs)]
        for r in recs:
            r[1:4] = list(np.array(r[1:4]) + verts[rng.randrange(len(verts))])
        return [(name, recs)]
    nodes = []

    def walk(i):
        b = nif.get(i)
        if b is None or "children" not in b:
            return
        nodes.append(b)
        for ch in b.get("children", []) or []:
            walk(ch)
    walk(c["emitter"])
    out = {}
    for r in recs:
        b = nodes[rng.randrange(len(nodes))]
        if flags & 8 and b is not em:
            # (the parent's place in this node's frame: back along its own transform)
            loc = nif_local(b)
            back = -loc[:3, :3].T @ loc[:3, 3]
            r[1:4] = list(np.array(r[1:4]) + back * rng.random())
        out.setdefault((b.get("name") or "").lower(), []).append(r)
    return sorted(out.items())


def make_creature_piece(crea_id, db, arch, ts):
    """A creature's particle sprays (an atronach's fire, a ghost's death spray, the Heart's bursts) as particle records
    in each emitter bone's own frame, born at times on the model's animation timeline: the engine sheds them into the
    world from the bone as the creature's animation passes each one (OpenMW's world-space particles). Returns
    {"base": piece name ('<id>_cpfx'), "meshes", "report"}, or None without sprays."""
    global TEXTURES
    from nif import Nif
    TEXTURES = ts
    rec = db["objects"][crea_id.lower()]
    model = rec.zstr("MODL")
    nif = Nif(arch.read("meshes/" + model))
    ss = sprays(nif, vis_hidden=True)
    if not ss:
        return None
    end = max([b["stop_time"] for b in nif.blocks if "stop_time" in b] + [1.0])
    meshes = []
    for sp in ss:
        c = sp["ctrl"]
        em = nif.get(c["emitter"])
        local = dict(sp, emitter=np.identity(4), initial=[])      # (records in the emitter's own frame)
        # a spray emitting longer than a few seconds (an atronach's fire, all its animations long): one second of it,
        # the engine sheds it again every second through its emit start .. stop
        repeat = None
        if c["emit_stop"] - c["emit_start"] > 3.0:
            repeat = (1.0, float(c["emit_start"]), float(c["emit_stop"]))
            local["ctrl"] = dict(c, emit_stop=c["emit_start"] + 1.0)
        recs = emit_records(local, min(end, local["ctrl"]["emit_stop"] + 0.01))
        # (where each particle leaves: the emitter node, or for a BSP array one of the nodes under it, picked at random)
        # (a creature's spray without a glow of its own is lit by the scene in OpenMW: half light matched the Flame
        # Atronach's fire side by side, full light burned white)
        glow = sp["mat"] is not None and max(sp["mat"]["emissive"]) > 0.0
        proto = sprite_meshes(arch, nif, [local], 1.0, still=True, light=1.0 if glow else 0.5)[0]
        for bone, part in bsp_targets(nif, c, recs, seed=sp["block"]):
            m = dict(proto, sprites=dict(proto["sprites"]))
            m["sprites"]["records"] = part
            if repeat:
                m["sprites"]["repeat"] = repeat
            m["bone"] = bone
            meshes.append((m, sp))
    # (each burst's live particles within the budget: thinned evenly, as the spells)
    # (a repeated spray's peak: as many as one second of it keeps alive, its life over a second)
    peak = peak_live([m["sprites"]["records"] for m, _ in meshes if "repeat" not in m["sprites"]], end) + sum(
        int(len(m["sprites"]["records"]) * max(1.0, m["sprites"]["records"][0][7] if m["sprites"]["records"] else 1.0))
        for m, _ in meshes if "repeat" in m["sprites"])
    total = sum(len(m["sprites"]["records"]) for m, _ in meshes)
    k = max(1, -(-peak // BUDGET), -(-total * 36 // (RECORDS_MAX * 2)))
    if k > 1:
        for m, _ in meshes:
            kept = m["sprites"]["records"][::k]
            for r in kept:
                r[8] *= k ** 0.5
            m["sprites"]["records"] = kept
    base = crea_id.lower().replace(" ", "_") + "_cpfx"
    n = sum(len(m["sprites"]["records"]) for m, _ in meshes)
    report = [f"{crea_id} ({model}): {len(meshes)} sprays on " + ", ".join(sorted({m['bone'] for m, _ in meshes})) +
          f" -> {n} particles ({n * 36 // 1024 + 1} KB), peak {peak} live" + (f", thinned 1 in {k}" if k > 1 else "")]
    return {"base": base, "meshes": [m for m, _ in meshes], "report": report}


def creature_auto(crea_id, out_dir, dry=False):
    """make_creature_piece on its own: the piece into out_dir/fp, listed under its game_firstperson.json "cpfx"."""
    from pathlib import Path
    import textures
    from firstperson import write_piece
    from mwfiles import Archives, load_db
    arch, db = Archives(), load_db()
    ts = textures.TextureSet(arch)
    r = make_creature_piece(crea_id, db, arch, ts)
    if r is None:
        print(f"{crea_id}: no sprays")
        return
    print(chr(10).join(r["report"]))
    if dry:
        return
    write_piece(Path(out_dir) / "fp" / f"{r['base']}.fpm", r["meshes"])
    ensure_textures(arch, ts, r["meshes"])
    edit_firstperson(out_dir, lambda fpj: fpj.setdefault("cpfx", {}).__setitem__(crea_id.lower(), r["base"]))


def main():
    from mwfiles import Archives
    from nif import Nif
    ap = argparse.ArgumentParser()
    ap.add_argument("--model")
    ap.add_argument("--png")
    ap.add_argument("--duration", type=float, default=1.2)
    ap.add_argument("--prototype", help="static id: write its flipbook / path pieces into --out")
    ap.add_argument("--sprites", help="static id: write its particle-record sprite piece into --out")
    ap.add_argument("--auto", help="static id: the best way spray by spray (flipbook / records), into --out")
    ap.add_argument("--creature", help="creature id: its sprays as bone-shed particle records, piece '<id>_cpfx'")
    ap.add_argument("--dry", action="store_true", help="--auto: only say what it would make")
    ap.add_argument("--out", default="out/world")
    a = ap.parse_args()
    if a.prototype:
        prototype(a.model, a.prototype, a.out)
        return
    if a.sprites:
        prototype_sprites(a.model, a.sprites, a.out)
        return
    if a.auto:
        auto(a.auto, a.out, a.dry)
        return
    if a.creature:
        creature_auto(a.creature, a.out, a.dry)
        return
    arch = Archives()
    nif = Nif(arch.read("meshes/" + a.model))
    ss = sprays(nif)
    sims = [simulate(s, a.duration) for s in ss]
    b = frames_bounds(sims)
    groups = {}
    for s, f in zip(ss, sims):
        groups.setdefault(s["additive"], []).append((f, particle_image(arch, nif, s)))
    blended = render_group(groups[False], False, b, 1.0) if False in groups else None
    added = render_group(groups[True], True, b, 1.0) if True in groups else None
    first = (blended or added)
    n, (w, h) = len(first), first[0].size
    sheet = Image.new("RGB", (w * n, h), (0, 0, 0))
    for k in range(n):
        frame = np.zeros((h, w, 3))
        if blended:
            bl = np.asarray(blended[k], dtype=float) / 255
            frame = bl[..., :3] * bl[..., 3:4] + frame * (1 - bl[..., 3:4])
        if added:
            frame = frame + np.asarray(added[k], dtype=float)[..., :3] / 255
        sheet.paste(Image.fromarray((np.clip(frame, 0, 1) * 255).astype(np.uint8)), (k * w, 0))
    sheet.save(a.png)
    print(f"{len(ss)} sprays, {n} frames, bounds {np.round(b, 1)}")


if __name__ == "__main__":
    main()
