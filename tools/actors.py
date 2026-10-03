"""Animated actors: skeleton + idle animation keyframes + per-NPC meshes, written to cells/<cell>.act.

Layout (little endian):
  'MWA1' u32 version=1
  u32 num_skeletons, per skeleton:
    u32 num_bones, per bone: char name[32], i32 parent, f32 rest[12] (3x4 rows: rotation*scale | translation)
    u32 num_tracks, per track: i32 bone,
        u32 n, n x (f32 t, f32 w, x, y, z)   rotation keys
        u32 n, n x (f32 t, f32 x, y, z)      translation keys
        u32 n, n x (f32 t, f32 s)            scale keys
    u32 num_groups, per group: char name[16], f32 start, loop_start, loop_stop, stop
  u32 num_actors, per actor: i32 ref, i32 skeleton, f32 place[12], u8 idle_chances[8] (Idle2..Idle9),
    u32 num_meshes, per mesh:
      i32 tex, u8 flags, u8 alpha_ref, u8 kind (0 rigid on bone, 1 skinned), u8 has_morph, i32 bone,
      u32 num_vertices, u32 num_indices,
      vertices (24 bytes: f32 pos[3] source position, f32 uv[2], u8 rgba[4]), u16 indices, pad to 4
      kind 1: u32 palette size, palette x (i32 bone, f32 skin_to_bone[12]), vertices x (u8 index[4], u8 weight[4])
      has_morph: f32 talk_start, talk_stop, blink_start, blink_stop, u32 num_morphs,
                 per morph: u32 n, n x (f32 t, f32 weight), f32 delta[num_vertices * 3]
Rigid source positions are in the bone's space; skinned ones in the mesh's skin space.
"""
import bisect
import struct

import numpy as np

from meshes import nif_local
from npc import MIRROR_X

IDLE_GROUPS = ["Idle"] + [f"Idle{i}" for i in range(2, 10)]
# Groups the 3DS plays besides the idles (name -> text key group), for combat and chasing
MOVE_GROUPS = ["RunForward", "WalkForward", "Hit1", "Death1", "KnockDown", "KnockOut"]
# Attacks: exported name -> text key group ("Chop ..." keys); the runtime picks one by weapon type.
# The group's hit moment is stored as its loop start.
ATTACK_GROUPS = {"AttackHH": "handtohand", "Attack1h": "weapononehand", "Attack2c": "weapontwohand",
                 "Attack2w": "weapontwowide"}
# Phased groups: exported name -> (text key group, start key, hit key, stop key). The hit moment
# (release / block hit) is stored as the loop start.
PHASED_GROUPS = {
    "AttackBow": ("bowandarrow", "shoot start", "shoot release", "shoot follow stop"),
    "AttackXbow": ("crossbow", "shoot start", "shoot release", "shoot follow stop"),
    "AttackThrow": ("throwweapon", "shoot start", "shoot release", "shoot follow stop"),
    "Block": ("shield", "block start", "block hit", "block stop"),
    "CastTarget": ("spellcast", "target start", "target release", "target stop"),
    "CastTouch": ("spellcast", "touch start", "touch release", "touch stop"),
    "CastSelf": ("spellcast", "self start", "self release", "self stop"),
}
FLAG_WEAPON = 128          # actor mesh drawn only while the actor fights


def mat34(m):
    return [float(v) for v in np.asarray(m)[:3, :4].ravel()]


def animation_groups(nif):
    """Idle groups first (the runtime picks idles among the leading "Idle*" groups), then
    MOVE_GROUPS and the attacks that the skeleton's text keys define."""
    keys = next((b["keys"] for b in nif.blocks if b["type"] == "NiTextKeyExtraData"), [])
    marks = {}
    for t, text in keys:
        for line in text.replace("\r", "").split("\n"):
            if ":" in line:
                group, what = (s.strip().lower() for s in line.split(":", 1))
                marks.setdefault((group, what), t)
    groups = []
    for g in IDLE_GROUPS + MOVE_GROUPS:
        gl = g.lower()
        if (gl, "start") in marks and (gl, "stop") in marks:
            start, stop = marks[(gl, "start")], marks[(gl, "stop")]
            groups.append({"name": g, "start": start, "stop": stop,
                           "loop_start": marks.get((gl, "loop start"), start),
                           "loop_stop": marks.get((gl, "loop stop"), stop)})
    # Creatures attack with Attack1 (its "Hit" key is when the blow lands)
    if ("attack1", "start") in marks and ("attack1", "stop") in marks:
        start, stop = marks[("attack1", "start")], marks[("attack1", "stop")]
        hit = marks.get(("attack1", "hit"), (start + stop) / 2)
        groups.append({"name": "Attack1", "start": start, "stop": stop, "loop_start": hit, "loop_stop": hit})
    for name, gl in ATTACK_GROUPS.items():
        start = marks.get((gl, "chop start"))
        stop = next((marks[(gl, k)] for k in ("chop large follow stop", "chop medium follow stop",
                                               "chop small follow stop") if (gl, k) in marks), None)
        if start is None or stop is None or stop <= start:
            continue
        hit = next((marks[(gl, k)] for k in ("chop hit", "chop min hit", "chop max attack") if (gl, k) in marks),
                   (start + stop) / 2)
        groups.append({"name": name, "start": start, "stop": stop, "loop_start": hit, "loop_stop": hit})
    for name, (gl, k0, kh, k1) in PHASED_GROUPS.items():
        start, hit, stop = marks.get((gl, k0)), marks.get((gl, kh)), marks.get((gl, k1))
        if start is not None and stop is not None and stop > start:
            hit = hit if hit is not None else (start + stop) / 2
            groups.append({"name": name, "start": start, "stop": stop, "loop_start": hit, "loop_stop": hit})
    return groups


def skeleton_export(nif, groups=None):
    """Bones, keyframe tracks (only keys inside the exported groups) and groups of a skeleton NIF;
    groups defaults to animation_groups(nif)."""
    keyframes = {}
    for i, b in enumerate(nif.blocks):
        c = b.get("controller", -1)
        while c is not None and c >= 0:
            cb = nif.get(c)
            if cb["type"] == "NiKeyframeController" and cb["data"] >= 0:
                keyframes[i] = nif.get(cb["data"])
            c = cb.get("next_controller", -1)

    groups = animation_groups(nif) if groups is None else groups
    intervals = [(g["start"], g["stop"]) for g in groups]
    bones, tracks, index = [], [], {}

    def in_range(keys):
        # keys inside the exported groups, plus the one before and after each for interpolation
        times = [k[0] for k in keys]
        keep = set()
        for s, e in intervals:
            i0 = max(bisect.bisect_right(times, s) - 1, 0)
            i1 = min(bisect.bisect_left(times, e), len(keys) - 1)
            keep.update(range(i0, i1 + 1))
        return [keys[i] for i in sorted(keep)]

    def walk(idx, parent):
        b = nif.get(idx)
        if b is None or "translation" not in b:
            return
        bi = len(bones)
        index[b["name"].lower()] = bi
        bones.append({"name": b["name"].lower(), "parent": parent, "rest": mat34(nif_local(b))})
        kd = keyframes.get(idx)
        if kd:
            tracks.append({"bone": bi, "rot": in_range(kd["rotations"]), "trans": in_range(kd["translations"]),
                           "scale": in_range(kd["scales"])})
        for c in b.get("children", []):
            walk(c, bi)

    for r in nif.roots:
        walk(r, -1)
    return {"bones": bones, "tracks": tracks, "groups": groups, "index": index}


def actor_mesh(s, skeleton, tex, colors):
    """Runtime mesh record from an NPC part shape (see npc.NpcBuilder.build)."""
    raw = s["raw"]
    n = len(raw["pos"])
    flags = s["flags"] | (FLAG_WEAPON if s.get("weapon") else 0)
    mesh = {"tex": tex, "flags": flags, "alpha_ref": s["alpha_ref"], "tris": s["tris"],
            "uv": s["uv"] * [1.0, -1.0] + [0.0, 1.0], "colors": colors, "bone": -1, "kind": 0}
    local = np.identity(4)
    if s["skinned"]:
        mesh["kind"] = 1
        mesh["pos"] = raw["pos"]
        palette, idx, w = [], np.zeros((n, 4), dtype=int), np.zeros((n, 4))
        for name, skin_to_bone, vi, vw in raw["skin"]:
            bi = skeleton["index"].get(name)
            if bi is None:
                continue
            pi = len(palette)
            palette.append((bi, mat34(skin_to_bone)))
            for v, weight in zip(vi, vw):
                slot = int(np.argmin(w[v]))      # keep the 4 strongest influences
                if weight > w[v, slot]:
                    idx[v, slot], w[v, slot] = pi, weight
        total = np.maximum(w.sum(axis=1, keepdims=True), 1e-6)
        wq = np.round(w / total * 255).astype(int)
        wq[:, 0] += 255 - wq.sum(axis=1)          # exact sum of 255 per vertex
        mesh["palette"], mesh["vidx"], mesh["vw"] = palette, idx, np.clip(wq, 0, 255)
    else:
        local = (MIRROR_X if s.get("mirror") else np.identity(4)) @ raw["node"]
        mesh["pos"] = raw["pos"] @ local[:3, :3].T + local[:3, 3]
        mesh["bone"] = skeleton["index"].get(s.get("bone") or "", -1)
    if "morph" in raw and len(raw["morph"]["morphs"]) > 1:
        md = raw["morph"]
        marks = {text.strip().lower(): t for t, text in raw["text_keys"]}
        mesh["morph"] = {
            "talk": (marks.get("talk: start", 0.0), marks.get("talk: stop", 0.0)),
            "blink": (marks.get("blink: start", 0.0), marks.get("blink: stop", 0.0)),
            "morphs": [(m["keys"], np.array(m["vectors"]).reshape(-1, 3) @ local[:3, :3].T)
                       for m in md["morphs"][1:]] if md["relative"] else [],
        }
    return mesh


def merge_meshes(meshes):
    """Fewer draw calls: skinned meshes sharing texture and render state are concatenated
    (palettes appended), rigid ones also need the same bone. Morphing heads stay separate."""
    groups, out = {}, []
    for m in meshes:
        if m.get("morph") and m["morph"]["morphs"]:
            out.append(m)
            continue
        key = (m["kind"], m["tex"], m.get("tex_name"), m["flags"], m["alpha_ref"], m["bone"] if m["kind"] == 0 else None)
        groups.setdefault(key, []).append(m)
    for key, ms in groups.items():
        if len(ms) == 1 or sum(len(m["pos"]) for m in ms) > 65000:
            out.extend(ms)
            continue
        merged = dict(ms[0])
        merged["pos"] = np.concatenate([m["pos"] for m in ms])
        merged["uv"] = np.concatenate([m["uv"] for m in ms])
        merged["colors"] = np.concatenate([m["colors"] for m in ms])
        base, tris = 0, []
        for m in ms:
            tris.append(np.asarray(m["tris"]) + base)
            base += len(m["pos"])
        merged["tris"] = np.concatenate(tris)
        if key[0] == 1:
            palette, vidx = [], []
            for m in ms:
                vidx.append(m["vidx"] + len(palette))
                palette += m["palette"]
            merged["palette"], merged["vidx"] = palette, np.concatenate(vidx)
            merged["vw"] = np.concatenate([m["vw"] for m in ms])
        out.append(merged)
    return out


def write_skeletons(path, skeletons):
    """Every skeleton the level's actors use, once: 'MWS1' u32 count, then skeletons as in .act v1."""
    with open(path, "wb") as f:
        f.write(b"MWS1" + struct.pack("<I", len(skeletons)))
        write_skeleton_list(f, skeletons)


def write_skeleton_list(f, skeletons):
    for sk in skeletons:
        f.write(struct.pack("<I", len(sk["bones"])))
        for b in sk["bones"]:
            f.write(b["name"].encode("latin-1")[:31].ljust(32, b"\x00"))
            f.write(struct.pack("<i12f", b["parent"], *b["rest"]))
        f.write(struct.pack("<I", len(sk["tracks"])))
        for t in sk["tracks"]:
            f.write(struct.pack("<iI", t["bone"], len(t["rot"])))
            for time, q in t["rot"]:
                f.write(struct.pack("<5f", time, *q))
            f.write(struct.pack("<I", len(t["trans"])))
            for time, v in t["trans"]:
                f.write(struct.pack("<4f", time, *v))
            f.write(struct.pack("<I", len(t["scale"])))
            for time, v in t["scale"]:
                f.write(struct.pack("<2f", time, v[0]))
        f.write(struct.pack("<I", len(sk["groups"])))
        for g in sk["groups"]:
            f.write(g["name"].encode("latin-1")[:15].ljust(16, b"\x00"))
            f.write(struct.pack("<4f", g["start"], g["loop_start"], g["loop_stop"], g["stop"]))


def write_mesh(f, m, tex_name=None):
    """One actor mesh; in library files (tex_name given) the texture goes by name."""
    n = len(m["pos"])
    if tex_name is None:
        f.write(struct.pack("<i", m["tex"]))
    else:
        name = tex_name.encode("latin-1")
        f.write(struct.pack("<H", len(name)) + name)
    f.write(struct.pack("<BBBBiII", m["flags"], m["alpha_ref"], m["kind"],
                        1 if m.get("morph") and m["morph"]["morphs"] else 0, m["bone"], n, len(m["tris"])))
    v = np.zeros(n, dtype=[("pos", "<f4", 3), ("uv", "<f4", 2), ("color", "u1", 4)])
    v["pos"], v["uv"], v["color"] = m["pos"], m["uv"], m["colors"]
    f.write(v.tobytes())
    f.write(np.asarray(m["tris"]).astype("<u2").tobytes())
    if len(m["tris"]) & 1:
        f.write(b"\x00\x00")
    if m["kind"] == 1:
        f.write(struct.pack("<I", len(m["palette"])))
        for bi, mat in m["palette"]:
            f.write(struct.pack("<i12f", bi, *mat))
        inf = np.concatenate([m["vidx"], m["vw"]], axis=1).astype(np.uint8)
        f.write(inf.tobytes())
    if m.get("morph") and m["morph"]["morphs"]:
        mo = m["morph"]
        f.write(struct.pack("<4fI", *mo["talk"], *mo["blink"], len(mo["morphs"])))
        for keys, delta in mo["morphs"]:
            f.write(struct.pack("<I", len(keys)))
            for t, wgt in keys:
                f.write(struct.pack("<2f", t, wgt))
            f.write(np.asarray(delta, dtype="<f4").tobytes())


def write_actor_library(path, meshes, tex_names):
    """'MWL1': one NPC / creature record's meshes (actor space, neutral lighting), shared by every
    placement: u32 version 1, u32 mesh count, meshes as in .act with the texture as u16 length + name."""
    with open(path, "wb") as f:
        f.write(b"MWL1" + struct.pack("<II", 1, len(meshes)))
        for m, name in zip(meshes, tex_names):
            write_mesh(f, m, name)


def write_actor_placements(path, actors):
    """.act version 3: per actor its reference, skeleton, placement, idles, light tint (rgb x 64, pad) and
    the library file with its meshes (u16 length + path under the data folder)."""
    with open(path, "wb") as f:
        f.write(b"MWA1" + struct.pack("<II", 3, 0))
        f.write(struct.pack("<I", len(actors)))
        for a in actors:
            f.write(struct.pack("<ii12f", a["ref"], a["skeleton"], *mat34(a["place"])))
            f.write(bytes(a["idle"]))
            f.write(bytes(a["tint"]) + b"\x00")
            lib = a["lib"].encode("latin-1")
            f.write(struct.pack("<H", len(lib)) + lib)


def write_actors(path, actors):
    """.act version 2: no skeletons inside; actors index into cells/skeletons.skl."""
    with open(path, "wb") as f:
        f.write(b"MWA1" + struct.pack("<II", 2, 0))
        f.write(struct.pack("<I", len(actors)))
        for a in actors:
            f.write(struct.pack("<ii12f", a["ref"], a["skeleton"], *mat34(a["place"])))
            f.write(bytes(a["idle"]))
            f.write(struct.pack("<I", len(a["meshes"])))
            for m in a["meshes"]:
                n = len(m["pos"])
                f.write(struct.pack("<iBBBBiII", m["tex"], m["flags"], m["alpha_ref"], m["kind"],
                                    1 if m.get("morph") and m["morph"]["morphs"] else 0, m["bone"], n, len(m["tris"])))
                v = np.zeros(n, dtype=[("pos", "<f4", 3), ("uv", "<f4", 2), ("color", "u1", 4)])
                v["pos"], v["uv"], v["color"] = m["pos"], m["uv"], m["colors"]
                f.write(v.tobytes())
                f.write(np.asarray(m["tris"]).astype("<u2").tobytes())
                if len(m["tris"]) & 1:
                    f.write(b"\x00\x00")
                if m["kind"] == 1:
                    f.write(struct.pack("<I", len(m["palette"])))
                    for bi, mat in m["palette"]:
                        f.write(struct.pack("<i12f", bi, *mat))
                    inf = np.concatenate([m["vidx"], m["vw"]], axis=1).astype(np.uint8)
                    f.write(inf.tobytes())
                if m.get("morph") and m["morph"]["morphs"]:
                    mo = m["morph"]
                    f.write(struct.pack("<4fI", *mo["talk"], *mo["blink"], len(mo["morphs"])))
                    for keys, delta in mo["morphs"]:
                        f.write(struct.pack("<I", len(keys)))
                        for t, wgt in keys:
                            f.write(struct.pack("<2f", t, wgt))
                        f.write(np.asarray(delta, dtype="<f4").tobytes())
