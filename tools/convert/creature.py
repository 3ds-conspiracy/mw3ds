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
from npc import item_parts, skeleton_pose, text_key_time
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


def key_at(keys, t):
    """A [(time, shown)] track's state at time t (its first state before its first key)."""
    on = keys[0][1]
    for kt, v in keys:
        if kt > t:
            break
        on = v
    return on


def and_keys(a, b):
    """Two [(time, shown)] tracks as one: shown while both are."""
    out = []
    for t in sorted({t for t, _ in a} | {t for t, _ in b}):
        on = key_at(a, t) and key_at(b, t)
        if not out or out[-1][1] != on:
            out.append((round(float(t), 4), on))
    return out


def emission_keys(ctrl, end):
    """A particle system's [(time, shown)]: from its emit start until its emit stop plus its particles' lifetime;
    None when it emits over the whole timeline (or past end, the model's own animations' end)."""
    start, stop = ctrl["emit_start"], ctrl["emit_stop"] + ctrl.get("lifetime", 0.0)
    begins = start <= ctrl.get("start_time", 0.0) + 0.01
    lasts = end is None or stop >= end
    if begins and lasts:
        return None
    out = [(0.0, begins)]
    if not begins:
        out.append((round(float(start), 4), True))
    if not lasts:
        out.append((round(float(stop), 4), False))
    return out


def vis_keys(nif, block, parents, end=None):
    """When a shape shows, from its own and its nodes' NiVisControllers (a Bonelord's robe and hood go at its death):
    [(time, shown)] on the model's animation timeline, shown only while every one of them is on; None without any.
    end: where the model's own animations stop. Past it (a biped creature's NPC groups, animlayers.layered) the shape
    is as it was before its keys: a Dremora's body, gone at the end of its deaths, is there for its idle and walk."""
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
    first = all(k[0][1] for k in tracks)
    if end is not None and out[-1][1] != first:
        out.append((round(float(end) + 0.5, 4), first))
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
        # (its own animations end at its last text key)
        end = max((k[0] for b in nif.blocks if b["type"] == "NiTextKeyExtraData" for k in b.get("keys", [])), default=None)
        for s in shapes:
            vis = vis_keys(nif, s.get("block", -1), parents, end)
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
                # It has particles only while it emits and they live on (an ancestor ghost's spray, an atronach's
                # burst); a fire that emits the whole timeline burns always
                if ctrl and "emit_start" in ctrl:
                    burst = emission_keys(ctrl, end)
                    if burst:
                        s["vis"] = and_keys(s.get("vis") or [(0.0, True)], burst)
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
            wid, _, sid = creature_weapon(rec, self.db, skills)
            wrec = self.db["objects"].get(wid.lower()) if wid else None
            wnif = self.npcs.nif("meshes\\" + wrec.zstr("MODL").lower()) if wrec is not None and wrec.zstr("MODL") else None
            wbone = pose.get("weapon bone")
            if wnif is not None and wbone is not None:
                for s in extract_shapes(self.arch, wnif):
                    s["bone"], s["mirror"], s["weapon"] = "weapon bone", False, True
                    s["pos"] = s["pos"] @ wbone[:3, :3].T + wbone[:3, 3]
                    s["nrm"] = s["nrm"] @ wbone[:3, :3].T
                    shapes.append(s)
            # Its shield rides its Shield Bone and shows always, a two-handed weapon aside (OpenMW's
            # CreatureWeaponAnimation, Slot_CarriedLeft: the shield's body part model, getShieldMesh)
            two_handed = wrec is not None and wrec.get("WPDT") and struct.unpack_from("<h", wrec.get("WPDT"), 8)[0] in (2, 4, 5, 6, 8, 9, 10)
            srec = self.db["objects"].get(sid.lower()) if sid and not two_handed else None
            spart = next((m for slot, m, _ in item_parts(srec) if slot == 10 and m), None) if srec is not None else None
            sbody = self.db["objects"].get(spart.lower()) if spart else None
            snif = self.npcs.nif("meshes\\" + sbody.zstr("MODL").lower()) if sbody is not None and sbody.zstr("MODL") else None
            sbone = pose.get("shield bone")
            if snif is not None and sbone is not None:
                for s in extract_shapes(self.arch, snif):
                    s["bone"], s["mirror"] = "shield bone", False
                    s["pos"] = s["pos"] @ sbone[:3, :3].T + sbone[:3, 3]
                    s["nrm"] = s["nrm"] @ sbone[:3, :3].T
                    shapes.append(s)
        xscl = rec.get("XSCL")
        scale = struct.unpack("<f", xscl)[0] if xscl else 1.0
        place = ref_matrix(ref) @ np.diag([scale, scale, scale, 1.0])
        for s in shapes:
            s["pos"] = s["pos"] @ place[:3, :3].T + place[:3, 3]
            s["nrm"] = s["nrm"] @ place[:3, :3].T
            s["nrm"] /= np.maximum(np.linalg.norm(s["nrm"], axis=1, keepdims=True), 1e-6)
        return shapes, {"skeleton": path, "place": place}
