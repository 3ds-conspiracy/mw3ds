"""NPC skeletons as Morrowind layers them: base_anim.nif carries every group; base_anim_female.nif only
re-does a few (Idle4, Idle5, WalkForward) and the rest still come from base_anim.nif. Exported alone,
the female file left every woman with three groups: no Idle, no running, attacking, dying or swimming
(they walked in place whenever told to stand). Also the groups the runtime asks for beyond
actors.MOVE_GROUPS: swimming and turning.

level.py calls npc_skeletons(conv) before writing cells/skel_N.skl. The female skeleton keeps the female
file's bone order (cached actor libraries index bones by it); base_anim's tracks are added by bone name
on a timeline after the female file's own keys. Not in the cell cache's code hash: changing it needs no
reconversion.
"""
import struct

from actors import IDLE_GROUPS, animation_groups, skeleton_export

BASE = "meshes\\base_anim.nif"
FEMALE = "meshes\\base_anim_female.nif"
BEAST = "meshes\\base_animkna.nif"
EXTRA = ["IdleSwim", "SwimWalkForward", "SwimRunForward", "SwimHit1", "SwimDeath", "SwimKnockDown", "SwimKnockOut",
         "TurnLeft", "TurnRight", "SwimTurnLeft", "SwimTurnRight"]


def text_marks(nif):
    keys = next((b["keys"] for b in nif.blocks if b["type"] == "NiTextKeyExtraData"), [])
    marks = {}
    for t, text in keys:
        for line in text.replace("\r", "").split("\n"):
            if ":" in line:
                group, what = (s.strip().lower() for s in line.split(":", 1))
                marks.setdefault((group, what), t)
    return marks


def groups_of(nif):
    """actors.animation_groups plus the swimming / turning groups the skeleton has."""
    groups = animation_groups(nif)
    have = {g["name"] for g in groups}
    marks = text_marks(nif)
    for g in EXTRA:
        gl = g.lower()
        if g not in have and (gl, "start") in marks and (gl, "stop") in marks:
            start, stop = marks[(gl, "start")], marks[(gl, "stop")]
            groups.append({"name": g, "start": start, "stop": stop, "loop_start": marks.get((gl, "loop start"), start),
                           "loop_stop": marks.get((gl, "loop stop"), stop)})
    return groups


def ordered(groups):
    """Idle, Idle2..Idle9 first and in that order (the runtime picks idles by position), then the rest."""
    idle = {g: k for k, g in enumerate(IDLE_GROUPS)}
    lead = sorted((g for g in groups if g["name"] in idle), key=lambda g: idle[g["name"]])
    return lead + [g for g in groups if g["name"] not in idle]


def layered(base_nif, over_nif):
    over_groups = groups_of(over_nif)
    names = {g["name"] for g in over_groups}
    base_groups = [g for g in groups_of(base_nif) if g["name"] not in names]
    o = skeleton_export(over_nif, over_groups)
    b = skeleton_export(base_nif, base_groups)
    times = [k[0] for t in o["tracks"] for kind in ("rot", "trans", "scale") for k in t[kind]]
    end = max(times + [g["stop"] for g in o["groups"]] + [0.0]) + 10.0
    by_bone = {t["bone"]: t for t in o["tracks"]}
    for t in b["tracks"]:
        bi = o["index"].get(b["bones"][t["bone"]]["name"])
        if bi is None:
            continue
        dst = by_bone.get(bi)
        if dst is None:
            dst = by_bone[bi] = {"bone": bi, "rot": [], "trans": [], "scale": []}
            o["tracks"].append(dst)
        for kind in ("rot", "trans", "scale"):
            dst[kind] = dst[kind] + [(k[0] + end,) + tuple(k[1:]) for k in t[kind]]
    shifted = [dict(g, start=g["start"] + end, stop=g["stop"] + end, loop_start=g["loop_start"] + end,
                    loop_stop=g["loop_stop"] + end) for g in base_groups]
    o["groups"] = ordered(o["groups"] + shifted)
    return o


def npc_skeletons(conv):
    """Re-exports the NPC skeletons among conv.skeletons (same bone order, so cached actors stay valid)."""
    fixed = []
    # Biped creatures (CREA flag 1): OpenMW layers xbase_anim (= base_anim's animation) under their own
    biped = {("meshes\\" + r.zstr("MODL")).lower() for r in conv.db["objects"].values()
             if r.tag == "CREA" and r.zstr("MODL") and r.get("FLAG") and struct.unpack_from("<i", r.get("FLAG"))[0] & 1}
    for path, k in conv.skel_index.items():
        p = path.lower()
        if p == FEMALE:
            conv.skeletons[k] = layered(conv.npcs.nif(BASE), conv.npcs.nif(path))
        elif p in (BASE, BEAST):
            conv.skeletons[k] = skeleton_export(conv.npcs.nif(path), ordered(groups_of(conv.npcs.nif(path))))
        elif p in biped and conv.npcs.nif(path) is not None:
            conv.skeletons[k] = layered(conv.npcs.nif(BASE), conv.npcs.nif(path))
        else:
            continue
        fixed.append(f"{path.rsplit(chr(92), 1)[-1]}: {len(conv.skeletons[k]['groups'])} groups")
    return fixed
