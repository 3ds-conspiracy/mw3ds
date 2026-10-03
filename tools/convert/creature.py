"""Creatures (mudcrabs, rats, kwama ...): their model is its own skeleton, skin and animation
(Idle, WalkForward, RunForward, Attack1, Hit1, Death1 text keys), so they go through the same
animated-actor path as NPCs (actors.py) with the model file as the skeleton.

Leveled creature references (LEVC) become the lowest-level creature in their list.
"""
import struct

import numpy as np

from meshes import extract_shapes, nif_local, ref_matrix
from npc import skeleton_pose, text_key_time
from npcstats import combat_spells


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


def creature_stats(rec, spel=None):
    """Actor fields for game.json (same shape as npcstats.npc_stats plus attack damage)."""
    d = rec.get("NPDT")
    v = struct.unpack_from("<24i", d) if d and len(d) >= 96 else [0, 1] + [30] * 8 + [20, 0, 50, 0, 30, 0, 0, 1, 3, 0, 0, 0, 0, 0]
    level, attrs = v[1], list(v[2:10])
    health, magicka, fatigue, combat, magic, stealth = v[10], v[11], v[12], v[14], v[15], v[16]
    attacks = [[v[17 + 2 * i], v[18 + 2 * i]] for i in range(3)]
    gold = v[23] if len(v) > 23 else 0
    aidt = rec.get("AIDT")
    aiw = rec.get("AI_W")
    return {"level": level, "attributes": attrs, "skills": [combat] * 9 + [magic] * 9 + [stealth] * 9,
            "health": max(health, 0), "magicka": max(magicka, 0),
            "fatigue": max(fatigue, 1), "gold": max(gold, 0), "fight": aidt[2] if aidt else 90,
            "flee": aidt[3] if aidt else 0, "combat_spells": combat_spells(rec, spel) if spel else [],
            "services": 0, "weapon": "", "armor": 0.0, "travel": [],
            "wander": struct.unpack_from("<H", aiw)[0] if aiw else 0,
            "creature": True, "attack": [a for a in attacks if a[1] > 0] or [[1, 3]]}


class CreatureBuilder:
    def __init__(self, arch, npcs):
        self.arch, self.npcs = arch, npcs          # NpcBuilder: shared NIF cache

    def build(self, rec, ref):
        """(world-space shapes at the idle pose, {"skeleton": path, "place": 4x4}) like NpcBuilder.build."""
        model = rec.zstr("MODL")
        path = "meshes\\" + model.lower() if model else None
        nif = self.npcs.nif(path) if path else None
        if nif is None:
            return [], None
        pose = skeleton_pose(nif, text_key_time(nif, "Idle: Start"))
        shapes = extract_shapes(self.arch, nif, skeleton=pose)
        parents = {}
        for i, b in enumerate(nif.blocks):
            for c in b.get("children", []):
                parents[c] = i
        for s in shapes:
            if s["skinned"] or "raw" not in s:
                continue
            # Rigid pieces ride the node they hang from; keep their own transform below it
            parent = nif.get(parents.get(s["block"], -1))
            s["bone"] = parent["name"].lower() if parent else None
            s["raw"]["node"] = nif_local(nif.get(s["block"]))
        xscl = rec.get("XSCL")
        scale = struct.unpack("<f", xscl)[0] if xscl else 1.0
        place = ref_matrix(ref) @ np.diag([scale, scale, scale, 1.0])
        for s in shapes:
            s["pos"] = s["pos"] @ place[:3, :3].T + place[:3, 3]
            s["nrm"] = s["nrm"] @ place[:3, :3].T
            s["nrm"] /= np.maximum(np.linalg.norm(s["nrm"], axis=1, keepdims=True), 1e-6)
        return shapes, {"skeleton": path, "place": place}
