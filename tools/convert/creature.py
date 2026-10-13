"""Creatures (mudcrabs, rats, kwama ...): their model is its own skeleton, skin and animation
(Idle, WalkForward, RunForward, Attack1, Hit1, Death1 text keys), so they go through the same
animated-actor path as NPCs (actors.py) with the model file as the skeleton.

Leveled creature references (LEVC) become the lowest-level creature in their list.

Creatures with the "Weapon and Shield" flag (skeletons, dremora, golden saints ...) fight with the weapon they
carry, as NPCs do: it rides their Weapon Bone and its damage replaces their attacks (OpenMW's Creature::hit).
"""
import struct

import numpy as np

from meshes import extract_shapes, nif_local, ref_matrix, walk_nif
from nif import Nif, NifError
from npc import skeleton_pose, text_key_time
from npcstats import best_shield, bind, combat_spells, combat_weapon


def resolve_leveled(db, lid, depth=0):
    """Creature record for a leveled list id (lowest level entry, following nested lists), or None."""
    entries = db["LEVC"].get(lid.lower())
    if not entries or depth > 4:
        return None
    for _, cid in sorted(entries):
        rec = db["objects"].get(cid.lower())
        if rec is not None and rec.tag == "CREA":
            return rec
        nested = resolve_leveled(db, cid, depth + 1)
        if nested is not None:
            return nested
    return None


CREA_WEAPON = 0x4                 # CREA FLAG: "Weapon and Shield", it uses the weapon and shield it carries


def creature_weapon(rec, db, skills):
    """(weapon, ammunition, shield) a creature fights with: only one with the Weapon and Shield flag, chosen
    from what it carries as for an NPC. Empty strings when it has none."""
    flag = rec.get("FLAG")
    if db is None or not flag or not struct.unpack("<i", flag)[0] & CREA_WEAPON:
        return "", "", ""
    bind(db)
    weapon, ammo = combat_weapon(rec, db["objects"], skills)
    return weapon or "", ammo or "", best_shield(rec, db["objects"]) or ""


_arch = None


def creature_box(rec):
    """The creature's collision box: OpenMW takes it from the model's "Bounding Box" node
    (BulletNifLoader::findBoundingBox: the bounds' center and half extents, the node's own transform left out)
    and rests the box's bottom on the ground under all of it (MovementSolver::traceDown sweeps the box down).
    [center x, center y, half x, half y, lift] in model units, lift being how far the origin stands above the
    box's bottom; None without such a node (OpenMW then builds a box up from the origin)."""
    global _arch
    model = rec.zstr("MODL")
    if not model:
        return None
    if _arch is None:
        from mwfiles import Archives
        _arch = Archives()
    path = "meshes\\" + model.lower()
    if not _arch.exists(path):
        return None
    try:
        nif = Nif(_arch.read(path))
    except NifError:
        return None
    for b in nif.blocks:
        box = b.get("bounds")
        if box and b.get("name", "").lower() == "bounding box" and all(e > 0 for e in box["extents"]):
            (cx, cy, cz), (ex, ey, ez) = box["center"], box["extents"]
            return [round(float(v), 2) for v in (cx, cy, ex, ey, ez - cz)]
    return None


def creature_stats(rec, spel=None, db=None):
    """Actor fields for game.json (same shape as npcstats.npc_stats plus attack damage)."""
    d = rec.get("NPDT")
    v = struct.unpack_from("<24i", d) if d and len(d) >= 96 else [0, 1] + [30] * 8 + [20, 0, 50, 0, 30, 0, 0, 1, 3, 0, 0, 0, 0, 0]
    level, attrs = v[1], list(v[2:10])
    health, magicka, fatigue, combat, magic, stealth = v[10], v[11], v[12], v[14], v[15], v[16]
    attacks = [[v[17 + 2 * i], v[18 + 2 * i]] for i in range(3)]
    gold = v[23] if len(v) > 23 else 0
    aidt = rec.get("AIDT")
    aiw = rec.get("AI_W")
    skills = [combat] * 9 + [magic] * 9 + [stealth] * 9
    weapon, ammo, shield = creature_weapon(rec, db, skills)
    return {"level": level, "attributes": attrs, "skills": skills,
            "health": max(health, 0), "magicka": max(magicka, 0),
            "fatigue": max(fatigue, 1), "gold": max(gold, 0), "fight": aidt[2] if aidt else 90,
            "flee": aidt[3] if aidt else 0, "combat_spells": combat_spells(rec, spel) if spel else [],
            "services": 0, "weapon": weapon, "ammo": ammo, "shield": shield, "armor": 0.0, "travel": [],
            "wander": struct.unpack_from("<H", aiw)[0] if aiw else 0,
            # (all three attacks, in order: Attack2 strikes with the second; [[1, 3]] for one with none)
            "creature": True, "attack": attacks if any(a[1] > 0 for a in attacks) else [[1, 3]], "box": creature_box(rec)}


def vis_keys(nif, block, parents):
    """When a shape shows, from its own and its nodes' NiVisControllers (a Bonelord's robe and hood go at its death):
    [(time, shown)] on the model's animation timeline, shown only while every one of them is on; None without any."""
    tracks = []
    i = block
    while i is not None and i >= 0:
        b = nif.get(i)
        c = nif.get(b.get("controller", -1)) if b else None
        while c:
            if c["type"] == "NiVisController" and nif.get(c["data"]) and nif.get(c["data"])["keys"]:
                tracks.append(nif.get(c["data"])["keys"])
            c = nif.get(c.get("next_controller", -1))
        i = parents.get(i)
    if not tracks:
        return None

    def at(keys, t):
        on = keys[0][1]
        for kt, v in keys:
            if kt > t:
                break
            on = v
        return on
    times = sorted({t for k in tracks for t, _ in k})
    out = []
    for t in times:
        on = all(at(k, t) for k in tracks)
        if not out or out[-1][1] != on:
            out.append((round(float(t), 4), on))
    return out


class CreatureBuilder:
    def __init__(self, arch, npcs, db=None):
        self.arch, self.npcs = arch, npcs          # NpcBuilder: shared NIF cache
        self.db = db                               # for the weapon a creature fights with

    def build(self, rec, ref):
        """(world-space shapes at the idle pose, {"skeleton": path, "place": 4x4}) like NpcBuilder.build."""
        model = rec.zstr("MODL")
        path = "meshes\\" + model.lower() if model else None
        nif = self.npcs.nif(path) if path else None
        if nif is None:
            return [], None
        pose = skeleton_pose(nif, text_key_time(nif, "Idle: Start"))
        # (with the parts its NiVisControllers show at times: they hide them the rest of the time, vis_keys)
        shapes = extract_shapes(self.arch, nif, skeleton=pose, vis_hidden=True)
        rest = {}
        walk_nif(nif, lambda b, m, p: rest.setdefault(b["index"], m), True)
        parents = {}
        for i, b in enumerate(nif.blocks):
            for c in b.get("children", []):
                parents[c] = i
        for s in shapes:
            vis = vis_keys(nif, s.get("block", -1), parents)
            if vis:
                s["vis"] = vis
            if "raw" not in s and s.get("block") is not None:
                # A particle system (a Flame Atronach's fire): its crossed quads (meshes.particle_shape, placed at
                # the emitter as the model holds it at rest) ride the node that emits them
                ctrl = nif.get(nif.get(s["block"]).get("controller", -1))
                emitter = nif.get(ctrl["emitter"]) if ctrl and "emitter" in ctrl else None
                at = rest.get(ctrl["emitter"]) if emitter else None
                if at is not None:
                    inv = np.linalg.inv(at)
                    s["raw"] = {"pos": np.asarray(s["pos"]) @ inv[:3, :3].T + inv[:3, 3], "node": np.identity(4)}
                    s["bone"] = emitter["name"].lower()
                continue
            if s["skinned"] or "raw" not in s:
                continue
            # Rigid pieces ride the node they hang from; keep their own transform below it
            parent = nif.get(parents.get(s["block"], -1))
            s["bone"] = parent["name"].lower() if parent else None
            s["raw"]["node"] = nif_local(nif.get(s["block"]))
        # The weapon it fights with rides its Weapon Bone, shown only while it fights (as an NPC's)
        if self.db is not None:
            d = rec.get("NPDT")
            v = struct.unpack_from("<24i", d) if d and len(d) >= 96 else None
            skills = [v[14]] * 9 + [v[15]] * 9 + [v[16]] * 9 if v else [30] * 27
            wid = creature_weapon(rec, self.db, skills)[0]
            wrec = self.db["objects"].get(wid.lower()) if wid else None
            wnif = self.npcs.nif("meshes\\" + wrec.zstr("MODL").lower()) if wrec is not None and wrec.zstr("MODL") else None
            wbone = pose.get("weapon bone")
            if wnif is not None and wbone is not None:
                for s in extract_shapes(self.arch, wnif):
                    s["bone"], s["mirror"], s["weapon"] = "weapon bone", False, True
                    s["pos"] = s["pos"] @ wbone[:3, :3].T + wbone[:3, 3]
                    s["nrm"] = s["nrm"] @ wbone[:3, :3].T
                    shapes.append(s)
        xscl = rec.get("XSCL")
        scale = struct.unpack("<f", xscl)[0] if xscl else 1.0
        place = ref_matrix(ref) @ np.diag([scale, scale, scale, 1.0])
        for s in shapes:
            s["pos"] = s["pos"] @ place[:3, :3].T + place[:3, 3]
            s["nrm"] = s["nrm"] @ place[:3, :3].T
            s["nrm"] /= np.maximum(np.linalg.norm(s["nrm"], axis=1, keepdims=True), 1e-6)
        return shapes, {"skeleton": path, "place": place}
