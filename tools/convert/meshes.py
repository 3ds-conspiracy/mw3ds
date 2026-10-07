"""Morrowind NIF -> flat triangle shapes, with rigid node transforms and skinning."""
import math
import re

import numpy as np

FLAG_BLEND, FLAG_TEST, FLAG_TWO_SIDED, FLAG_CLAMP, FLAG_ADDITIVE = 1, 2, 4, 8, 16
# Set by the cell converter only: coplanar overlay (terrain layers), scrolling UVs (water, clouds)
FLAG_DECAL, FLAG_SCROLL = 32, 64
# Smoke, steam and spark particles: the texture rises over time (the quads themselves are static); a flame is RISE
# with CLAMP: its picture flickers instead
FLAG_RISE = 128
ALPHA_ONE = 0  # NiAlphaProperty blend factor


# ---- transforms ----

def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def ref_matrix(ref):
    rx, ry, rz = ref["rot"]
    m = np.identity(4)
    m[:3, :3] = rot_x(-rx) @ rot_y(-ry) @ rot_z(-rz) * ref["scale"]
    m[:3, 3] = ref["pos"]
    return m


def nif_local(b):
    m = np.identity(4)
    if "rotation" in b:
        m[:3, :3] = np.array(b["rotation"]).reshape(3, 3) * b["scale"]
        m[:3, 3] = b["translation"]
    return m


# ---- NIF -> shapes ----

def walk_nif(nif, visit):
    """Calls visit(block, world_matrix, properties) for every visible AV object."""
    def walk(idx, parent, props):
        b = nif.get(idx)
        if b is None or b["type"] == "RootCollisionNode" or b.get("flags", 0) & 1:
            return
        m = parent @ nif_local(b)
        p = props + [nif.get(i) for i in b.get("properties", []) if nif.get(i)]
        visit(b, m, p)
        children = b.get("children", [])
        if b["type"] == "NiSwitchNode":
            children = children[b["switch_index"]:b["switch_index"] + 1]
        elif b["type"] == "NiLODNode":
            children = children[:1]
        for c in children:
            walk(c, m, p)
    for r in nif.roots:
        walk(r, np.identity(4), [])


def last_prop(props, ptype):
    for p in reversed(props):
        if p["type"] == ptype:
            return p
    return None


def strips_to_triangles(strips):
    tris = []
    for s in strips:
        for i in range(len(s) - 2):
            a, b, c = s[i], s[i + 1], s[i + 2]
            if i & 1:
                a, b = b, a
            if a != b and b != c and a != c:
                tris += (a, b, c)
    return tris


def texture_path(arch, nif, texprop):
    if not texprop or not texprop["textures"] or not texprop["textures"][0]:
        return None, 0, 3
    desc = texprop["textures"][0]
    src = nif.get(desc["source"])
    if not src or not src.get("file"):
        return None, 0, 3
    name = src["file"].lower().replace("/", "\\")
    if not name.startswith("textures\\"):
        name = "textures\\" + name
    dds = re.sub(r"\.[^.\\]+$", ".dds", name)
    for cand in (dds, name):
        if arch.exists(cand):
            return cand, desc["uv_set"], desc["clamp"]
    return None, 0, 3


def transform_matrix(t):
    m = np.identity(4)
    m[:3, :3] = np.array(t["rotation"]).reshape(3, 3) * t["scale"]
    m[:3, 3] = t["translation"]
    return m


def skin_vertices(nif, inst, pos, nrm, skeleton):
    """Blends vertices over the skeleton's posed bones (matched by name).
    Result is in skeleton space: sum(w * boneWorld * skinToBone * v)."""
    data = nif.get(inst["data"])
    out_p, out_n = np.zeros_like(pos), np.zeros_like(nrm)
    total = np.zeros(len(pos))
    for bone_ref, bone in zip(inst["bones"], data["bones"]):
        node = nif.get(bone_ref)
        world = skeleton.get(node["name"].lower()) if node else None
        if world is None or not bone["indices"]:
            continue
        m = world @ transform_matrix(bone["transform"])
        idx = np.array(bone["indices"], dtype=int)
        w = np.array(bone["weights"])[:, None]
        out_p[idx] += w * (pos[idx] @ m[:3, :3].T + m[:3, 3])
        out_n[idx] += w * (nrm[idx] @ m[:3, :3].T)
        total[idx] += w[:, 0]
    if np.any(total < 0.5):
        print(f"    warning: {np.sum(total < 0.5)} vertices without bone weights")
    return out_p, out_n


def extract_shapes(arch, nif, skeleton=None, bind_pose=False):
    """Flattens a NIF into shapes. Rigid shapes come out in the NIF's root space;
    skinned shapes are skipped unless `skeleton` (bone name -> posed matrix) is given,
    then they come out in skeleton space and are marked "skinned". `bind_pose` draws a skinned
    shape as the NIF's own nodes hold it (a banner's cloth hangs off bones the game animates;
    placed as a static it is drawn at rest)."""
    shapes = []

    node_world = {}
    walk_nif(nif, lambda b, m, p: node_world.setdefault(b["index"], m))
    if bind_pose and skeleton is None:
        skeleton = {nif.get(i)["name"].lower(): m for i, m in node_world.items() if nif.get(i)}

    def visit(b, m, props):
        if b["type"] in ("NiRotatingParticles", "NiAutoNormalParticles"):
            s = particle_shape(arch, nif, b, props, node_world)
            if s:
                shapes.append(s)
            return
        if b["type"] not in ("NiTriShape", "NiTriStrips"):
            return
        if b["skin"] != -1 and skeleton is None:
            return
        if b["name"].lower().startswith(("tri editormarker", "tri shadow", "shadow")):
            return
        d = nif.get(b["data"])
        if not d or not d["vertices"]:
            return
        n = d["num_vertices"]
        tris = d["triangles"] if "triangles" in d else strips_to_triangles(d["strips"])
        if not tris:
            return
        tex, uv_set, clamp = texture_path(arch, nif, last_prop(props, "NiTexturingProperty"))
        pos = np.array(d["vertices"]).reshape(n, 3)
        nrm = np.array(d["normals"]).reshape(n, 3) if d["normals"] else np.tile([0.0, 0.0, 1.0], (n, 1))
        raw = {"pos": pos.copy(), "node": m.copy()}     # what animated actors need at run time
        if b["skin"] != -1:
            inst = nif.get(b["skin"])
            data = nif.get(inst["data"])
            raw["skin"] = [(nif.get(bref)["name"].lower(), transform_matrix(bone["transform"]),
                            np.array(bone["indices"], dtype=int), np.array(bone["weights"]))
                           for bref, bone in zip(inst["bones"], data["bones"]) if nif.get(bref)]
        c = nif.get(b.get("controller", -1))
        while c:
            if c["type"] == "NiGeomMorpherController" and nif.get(c["data"]):
                raw["morph"] = nif.get(c["data"])
                raw["text_keys"] = next((x["keys"] for x in nif.blocks if x["type"] == "NiTextKeyExtraData"), [])
            c = nif.get(c.get("next_controller", -1))
        if b["skin"] != -1:
            pos, nrm = skin_vertices(nif, nif.get(b["skin"]), pos, nrm, skeleton)
        else:
            pos = pos @ m[:3, :3].T + m[:3, 3]
            nrm = nrm @ m[:3, :3].T
        nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
        uvs = d["uv_sets"]
        uv = np.array(uvs[min(uv_set, len(uvs) - 1)]).reshape(n, 2) if uvs else np.zeros((n, 2))
        alpha = last_prop(props, "NiAlphaProperty")
        stencil = last_prop(props, "NiStencilProperty")
        flags = 0
        if alpha and alpha["flags"] & 1:
            flags |= FLAG_BLEND
            if (alpha["flags"] >> 5) & 15 == ALPHA_ONE:
                flags |= FLAG_ADDITIVE
        if alpha and alpha["flags"] & 0x200:
            flags |= FLAG_TEST
        if stencil and stencil["draw_mode"] == 3:
            flags |= FLAG_TWO_SIDED
        if clamp != 3:
            flags |= FLAG_CLAMP
        shapes.append({
            "pos": pos, "nrm": nrm, "uv": uv,
            "vcol": np.array(d["colors"]).reshape(n, 4) if d["colors"] else None,
            "tris": np.array(tris, dtype=np.uint32),
            "tex": tex, "flags": flags, "skinned": b["skin"] != -1, "name": b["name"], "block": b["index"],
            "alpha_ref": alpha["threshold"] if alpha else 0,
            "mat": last_prop(props, "NiMaterialProperty"),
            "vcp": last_prop(props, "NiVertexColorProperty"),
            "raw": raw,
        })

    walk_nif(nif, visit)
    return shapes


def find_node(nif, name):
    found = []
    walk_nif(nif, lambda b, m, p: found.append(m) if b.get("name", "").lower() == name else None)
    return found[0] if found else None


def particle_color(nif, ctrl):
    mod = nif.get(ctrl.get("particle_modifier", -1))
    while mod:
        if mod["type"] == "NiParticleColorModifier":
            data = nif.get(mod["color_data"])
            if data and data["keys"]:
                return np.array(data["keys"][0][1])
        mod = nif.get(mod.get("next_modifier", -1))
    return np.array(ctrl["initial_color"])


def particle_shape(arch, nif, b, props, node_world):
    """Approximates a particle emitter (candle / torch flame) as three crossed vertical quads
    at the emitter, sized from the particle settings, drawn unlit with the particles' blending."""
    ctrl = nif.get(b["controller"])
    if not ctrl or "emitter" not in ctrl:
        return None
    emitter = node_world.get(ctrl["emitter"])
    tex, _, _ = texture_path(arch, nif, last_prop(props, "NiTexturingProperty"))
    if emitter is None or tex is None:
        return None
    size = max(ctrl["initial_size"], 1.0)
    rise = ctrl["speed"] * ctrl["lifetime"]
    half_w, height = size, 2 * size + 0.5 * rise
    color = particle_color(nif, ctrl)

    pos, uv, tris = [], [], []
    for k in range(3):
        a = k * math.pi / 3
        dx, dy = math.cos(a) * half_w, math.sin(a) * half_w
        base = len(pos)
        pos += [(-dx, -dy, -size * 0.5), (dx, dy, -size * 0.5), (dx, dy, height), (-dx, -dy, height)]
        uv += [(0, 1), (1, 1), (1, 0), (0, 0)]
        tris += [base, base + 1, base + 2, base, base + 2, base + 3]
    pos = np.array(pos, dtype=float)
    # Emitter origin, but keep the flame upright in its parent's frame
    pos = pos + emitter[:3, 3]
    n = len(pos)

    alpha = last_prop(props, "NiAlphaProperty")
    flags = FLAG_TWO_SIDED | FLAG_BLEND
    if alpha and (alpha["flags"] >> 5) & 15 == ALPHA_ONE:
        flags |= FLAG_ADDITIVE
    name = tex.lower()
    if re.search("smoke|steam|spark", name):
        flags |= FLAG_RISE
    elif re.search("fire|flame", name) and "ash" not in name:
        flags |= FLAG_RISE | FLAG_CLAMP     # a flame: its picture flickers (no climb)
    return {
        "pos": pos, "nrm": np.tile([0.0, 0.0, 1.0], (n, 1)), "uv": np.array(uv, dtype=float),
        "vcol": np.tile(np.append(color[:3], 1.0), (n, 1)),
        "tris": np.array(tris, dtype=np.uint32), "tex": tex, "flags": flags, "skinned": False,
        "alpha_ref": 0, "mat": None, "vcp": {"vertex_mode": 1, "lighting_mode": 0},
    }


def has_no_collision(nif):
    """Root NiStringExtraData starting with "NC" (e.g. NCO) marks meshes actors walk through."""
    for r in nif.roots:
        root = nif.get(r)
        e = nif.get(root.get("extra", -1)) if root else None
        while e:
            if e["type"] == "NiStringExtraData" and e["string"].upper().startswith("NC"):
                return True
            e = nif.get(e.get("next_extra", -1))
    return False


def collision_triangles(nif):
    """World-space-ready collision soup in the NIF's root space: (Nx3 positions, Mx3 indices).
    Uses RootCollisionNode geometry when present (hidden flags ignored there),
    otherwise the visible, unskinned geometry."""
    if has_no_collision(nif):
        return None
    pos_list, tri_list, count = [], [], 0

    def add(b, m):
        nonlocal count
        if b["type"] not in ("NiTriShape", "NiTriStrips") or b["skin"] != -1:
            return
        d = nif.get(b["data"])
        if not d or not d["vertices"]:
            return
        tris = d["triangles"] if "triangles" in d else strips_to_triangles(d["strips"])
        if not tris:
            return
        p = np.array(d["vertices"]).reshape(-1, 3)
        pos_list.append(p @ m[:3, :3].T + m[:3, 3])
        tri_list.append(np.array(tris, dtype=np.int64).reshape(-1, 3) + count)
        count += len(p)

    rcn = [i for i, b in enumerate(nif.blocks) if b["type"] == "RootCollisionNode"]
    if rcn:
        parents = {}
        for i, b in enumerate(nif.blocks):
            for c in b.get("children", []):
                parents[c] = i

        def world(i):
            m = np.identity(4)
            while i is not None:
                m = nif_local(nif.get(i)) @ m
                i = parents.get(i)
            return m

        def walk_all(i, m):
            b = nif.get(i)
            if b is None:
                return
            add(b, m)
            for c in b.get("children", []):
                cb = nif.get(c)
                if cb is not None:
                    walk_all(c, m @ nif_local(cb))
        for i in rcn:
            walk_all(i, world(i))
    else:
        walk_nif(nif, lambda b, m, p: add(b, m) if not b["name"].lower().startswith(
            ("tri editormarker", "tri shadow", "shadow")) else None)
    if not pos_list:
        return None
    return np.concatenate(pos_list), np.concatenate(tri_list)
