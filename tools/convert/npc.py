"""Builds static NPC geometry the way Morrowind assembles actors:
race skin body parts + the NPC's head and hair + equipped clothing and armor,
attached to the base_anim skeleton posed at the first frame of the idle animation.

Skinned parts are blended over the skeleton's bones by name. Rigid parts hang
off the bone for their slot; parts on "Left ..." bones reuse right-side meshes
mirrored in X, as the original engine does.
"""
import struct

import numpy as np

from meshes import extract_shapes, nif_local, ref_matrix
from nif import Nif
from npcstats import bind, npc_items, npc_stats

# Part reference slots (ESM INDX values) -> skeleton bone they attach to
PART_BONES = [
    "Head", "Head", "Neck", "Chest", "Groin", "Groin", "Right Hand", "Left Hand",
    "Right Wrist", "Left Wrist", "Shield Bone", "Right Forearm", "Left Forearm",
    "Right Upper Arm", "Left Upper Arm", "Right Foot", "Left Foot", "Right Ankle",
    "Left Ankle", "Right Knee", "Left Knee", "Right Upper Leg", "Left Upper Leg",
    "Right Clavicle", "Left Clavicle", "Weapon Bone", "Tail",
]
SLOT_HEAD, SLOT_HAIR, SLOT_WEAPON = 0, 1, 25

# BODY record part type -> slots it fills for skin parts
SKIN_SLOTS = {2: [2], 3: [3], 4: [4], 5: [6, 7], 6: [8, 9], 7: [11, 12], 8: [13, 14],
              9: [15, 16], 10: [17, 18], 11: [19, 20], 12: [21, 22], 13: [23, 24], 14: [26]}

MIRROR_X = np.diag([-1.0, 1.0, 1.0, 1.0])

# Robes and skirts cover these slots, as in the original engine
CLOT_ROBE, CLOT_SKIRT = 4, 7
ROBE_HIDES = [3, 4, 5, 11, 12, 13, 14, 19, 20, 21, 22]
SKIRT_HIDES = [4, 21, 22]


def quat_matrix(q):
    w, x, y, z = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ])


def sample(keys, t, quat=False):
    """Key value at time t: linear between keys (normalized for quaternions), clamped at the ends."""
    if t <= keys[0][0]:
        v = np.array(keys[0][1])
    elif t >= keys[-1][0]:
        v = np.array(keys[-1][1])
    else:
        for (t0, v0), (t1, v1) in zip(keys, keys[1:]):
            if t <= t1:
                f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
                a, b = np.array(v0), np.array(v1)
                if quat and np.dot(a, b) < 0:
                    b = -b
                v = a * (1 - f) + b * f
                break
    return v / np.linalg.norm(v) if quat else v


def text_key_time(nif, name):
    for b in nif.blocks:
        if b["type"] == "NiTextKeyExtraData":
            for t, text in b["keys"]:
                if text.lower().startswith(name.lower()):
                    return t
    return 0.0


def skeleton_pose(nif, t):
    """Bone name (lowercase) -> 4x4 skeleton-space matrix, with keyframes sampled at time t."""
    keyframes = {}
    for i, b in enumerate(nif.blocks):
        c = b.get("controller", -1)
        while c is not None and c >= 0:
            cb = nif.get(c)
            if cb["type"] == "NiKeyframeController" and cb["data"] >= 0:
                keyframes[i] = nif.get(cb["data"])
            c = cb.get("next_controller", -1)

    bones = {}

    def walk(idx, parent):
        b = nif.get(idx)
        if b is None or "translation" not in b:
            return
        local = nif_local(b)
        kd = keyframes.get(idx)
        if kd:
            rot = quat_matrix(sample(kd["rotations"], t, quat=True)) if kd["rotations"] \
                else np.array(b["rotation"]).reshape(3, 3)
            trans = sample(kd["translations"], t) if kd["translations"] else b["translation"]
            scale = sample(kd["scales"], t)[0] if kd["scales"] else b["scale"]
            local = np.identity(4)
            local[:3, :3] = rot * scale
            local[:3, 3] = trans
        m = parent @ local
        bones.setdefault(b["name"].lower(), m)
        for c in b.get("children", []):
            walk(c, m)

    for r in nif.roots:
        walk(r, np.identity(4))
    return bones


def item_parts(rec):
    """(slot, male part id, female part id) for an ARMO/CLOT record."""
    parts, cur = [], None
    for tag, d in rec.subs:
        if tag == "INDX":
            cur = [d[0], None, None]
            parts.append(cur)
        elif tag in ("BNAM", "CNAM") and cur is not None:
            cur[1 if tag == "BNAM" else 2] = d.split(b"\0", 1)[0].decode("latin-1")
    return parts


class NpcBuilder:
    def __init__(self, arch, objects, races, bodies, db=None):
        self.arch, self.objects, self.races, self.bodies = arch, objects, races, bodies
        self.db = db                    # for the weapon they fight with (npcstats)
        self.nifs, self.skeletons = {}, {}

    def nif(self, path):
        key = path.lower()
        if key not in self.nifs:
            self.nifs[key] = Nif(self.arch.read(key)) if self.arch.exists(key) else None
        return self.nifs[key]

    def skeleton(self, path):
        if path not in self.skeletons:
            nif = self.nif(path)
            self.skeletons[path] = skeleton_pose(nif, text_key_time(nif, "Idle: Start"))
        return self.skeletons[path]

    def parts(self, npc, race_id, female):
        parts = [None] * len(PART_BONES)
        # Own sex first, then (women only) male parts for what is still missing, as in OpenMW's
        # getBodyParts: the female Argonian has no forearm part
        for sex in ([1, 0] if female else [0]):
            for body in self.bodies:
                part, vampire, flags, kind = body.get("BYDT")[:4]
                if kind != 0 or vampire or (flags & 1) != sex or body.id.lower().endswith(".1st"):
                    continue
                if (body.zstr("FNAM") or "").lower() != race_id:
                    continue
                for slot in SKIN_SLOTS.get(part, []):
                    parts[slot] = parts[slot] or body.id
        parts[SLOT_HEAD] = npc.zstr("BNAM")
        parts[SLOT_HAIR] = npc.zstr("KNAM")
        if self.db:
            bind(self.db)
        items = [self.objects.get(item.lower()) for _, item in npc_items(npc)]

        def clothing_type(rec):
            return struct.unpack_from("<i", rec.get("CTDT"))[0] if rec.tag == "CLOT" else -1

        # A helmet on the head takes the hair away (a coif that has a hair part of its own puts that back below):
        # hair and the ears of some hair models stuck out of the Dark Elf guards' helmets
        if any(r and r.tag == "ARMO" and struct.unpack_from("<i", r.get("AODT"))[0] == 0 for r in items):
            parts[SLOT_HAIR] = None
        # Clothing, then armor over it, then skirts, then robes on top of everything
        layers = [[r for r in items if r and r.tag == "CLOT" and clothing_type(r) not in (CLOT_ROBE, CLOT_SKIRT)],
                  [r for r in items if r and r.tag == "ARMO"],
                  [r for r in items if r and r.tag == "CLOT" and clothing_type(r) == CLOT_SKIRT],
                  [r for r in items if r and r.tag == "CLOT" and clothing_type(r) == CLOT_ROBE]]
        for layer in layers:
            for rec in layer:
                for slot in {CLOT_ROBE: ROBE_HIDES, CLOT_SKIRT: SKIRT_HIDES}.get(clothing_type(rec), []):
                    parts[slot] = None
                for slot, male, fem in item_parts(rec):
                    if slot < len(parts):
                        parts[slot] = fem if female and fem else male   # no part id = slot hidden
        return parts

    def build(self, npc, ref):
        """(world-space shapes at the idle pose, {"skeleton": path, "place": 4x4}) for an NPC reference.
        Shapes keep their raw data (see meshes.extract_shapes) plus "bone"/"mirror" for rigid parts."""
        race = self.races.get((npc.zstr("RNAM") or "").lower())
        if race is None:
            return [], None
        female = struct.unpack("<I", npc.get("FLAG"))[0] & 1
        radt = race.get("RADT")
        height = struct.unpack_from("<f", radt, 120 + 4 * female)[0]
        weight = struct.unpack_from("<f", radt, 128 + 4 * female)[0]
        beast = struct.unpack_from("<i", radt, 136)[0] & 2
        skel_path = "meshes\\base_animkna.nif" if beast else (
            "meshes\\base_anim_female.nif" if female and self.arch.exists("meshes\\base_anim_female.nif")
            else "meshes\\base_anim.nif")
        bones = self.skeleton(skel_path)

        shapes, seen = [], set()
        for slot, pid in enumerate(self.parts(npc, race.id.lower(), female)):
            if pid is None or slot == SLOT_WEAPON:
                continue
            body = self.objects.get(pid.lower())
            model = body.zstr("MODL") if body else None
            nif = self.nif("meshes\\" + model) if model else None
            if nif is None:
                continue
            bone_name = PART_BONES[slot]
            bone = bones.get(bone_name.lower())
            mirror = bone_name.startswith("Left")
            for i, s in enumerate(extract_shapes(self.arch, nif, skeleton=bones)):
                if s["skinned"]:
                    # Skinned part files can hold pieces for several slots (e.g. both hands
                    # plus a chest); a slot only takes the pieces named "Tri <bone>"
                    name = s["name"].lower()
                    if not name.startswith(("tri " + bone_name.lower(), "tril" + bone_name.lower())):
                        continue
                    if (model.lower(), i) in seen:
                        continue
                    seen.add((model.lower(), i))
                elif bone is not None:
                    s["bone"], s["mirror"] = bone_name.lower(), mirror
                    m = bone @ MIRROR_X if mirror else bone
                    s["pos"] = s["pos"] @ m[:3, :3].T + m[:3, 3]
                    s["nrm"] = s["nrm"] @ m[:3, :3].T
                    if mirror:
                        s["tris"] = s["tris"].reshape(-1, 3)[:, [0, 2, 1]].ravel()
                shapes.append(s)

        # Their best weapon rides the weapon bone; the 3DS shows it only while they fight
        wid = npc_stats(npc, self.db)["weapon"] if self.db else None
        wrec = self.objects.get(wid.lower()) if wid else None
        wnif = self.nif("meshes\\" + wrec.zstr("MODL")) if wrec is not None and wrec.zstr("MODL") else None
        wbone = bones.get("weapon bone")
        if wnif is not None and wbone is not None:
            for s in extract_shapes(self.arch, wnif):
                s["bone"], s["mirror"], s["weapon"] = "weapon bone", False, True
                s["pos"] = s["pos"] @ wbone[:3, :3].T + wbone[:3, 3]
                s["nrm"] = s["nrm"] @ wbone[:3, :3].T
                shapes.append(s)

        place = ref_matrix(ref) @ np.diag([weight, weight, height, 1.0])
        for s in shapes:
            s["pos"] = s["pos"] @ place[:3, :3].T + place[:3, 3]
            s["nrm"] = s["nrm"] @ place[:3, :3].T
            s["nrm"] /= np.maximum(np.linalg.norm(s["nrm"], axis=1, keepdims=True), 1e-6)
        return shapes, {"skeleton": skel_path, "place": place}

