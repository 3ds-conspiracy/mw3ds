"""First-person view: the player's arms and what they hold, drawn over the world.

Morrowind draws the player in first person with its own skeleton (base_anim.1st.nif) carrying hands (".1st" body parts when they exist),
wrists, forearms and upper arms, the weapon on "Weapon Bone" and the shield on "Shield Bone".
What the player wears changes while playing, so every part and item model is converted on its
own and the 3DS puts the view model together from the equipped items.

Writes data/fp/ (zlib-wrapped like the cells):
  skeletons.skl   'MWS1' (actors.py layout): the first-person skeleton with the FP groups
  <piece>.fpm     'MWP1' u32 num_textures, 64-byte names (relative to data/textures),
                  u32 num_meshes, per mesh:
                    i32 tex, u8 flags, u8 alpha_ref, u8 kind (0 rigid, 1 skinned), u8 0,
                    char bone[32] (rigid: bone it rides, "" = root), u32 num_vertices, u32 num_indices,
                    vertices (24 bytes: f32 pos[3], f32 uv[2], u8 rgba[4]), u16 indices (pad to 4),
                    (the u8 after kind, bits: 1 the mesh morphs, its morphs last: u32 count, per morph u32 n,
                    n x (f32 t, f32 weight), f32 delta[num_vertices * 3]; 2 it moves, after that: u32 n,
                    n x f32 matrix[12] (3x4 rows, its space to the root, VFX_FPS a second from 0: a spell
                    visual's node keyframes); 4 it turns to face the eye (a NiBillboardNode); 8 it takes the magic
                    effect's particle texture (game.json "firstperson" "ptex": effect index -> texture); 32 a
                    flipbook, 64 baked particle paths (particles.py, after the track); 128 more after those: u8,
                    2 a texture slide, 4 a fade (both sampled VFX_FPS a second), 1 particle records)
                    kind 1: u32 palette size, per entry: char bone[32], f32 skin_to_bone[12];
                            per vertex u8 index[4], u8 weight[4]
Rigid positions are in their bone's space, skinned ones in skin space; bones are named, so a
piece fits either skeleton. game.json "firstperson" says which piece goes where:
  {"races": {race: {"m": {slot: piece}, "f": {...}}},
   "items": {item: {"slots": {slot: [male piece, female piece]}, "model": piece}}}
Slots are body part slots (npc.PART_BONES): 6/7 hands, 8/9 wrists, 10 shield, 11/12 forearms,
13/14 upper arms, 25 weapon. "model" is an item's own mesh at its root (arrows in flight, dropped items).
"""
import math
import re
import struct

import numpy as np

from actors import skeleton_export, write_skeleton_list
from meshes import extract_shapes, walk_nif
from npc import MIRROR_X, PART_BONES, SKIN_SLOTS, item_parts, quat_matrix, sample, skeleton_pose, text_key_time
from nif import Nif
from textures import texels_needed, world_per_repeat

# Beast races' own first-person skeleton only animates hand-to-hand; their hands fit this one
SKELETONS = ["meshes\\base_anim.1st.nif"]
ARM_SLOTS = [6, 7, 8, 9, 10, 11, 12, 13, 14]
ITEM_TAGS = ("WEAP", "ARMO", "CLOT", "MISC", "BOOK", "ALCH", "INGR", "APPA", "LOCK", "PROB", "REPA", "LIGH")
SLOT_SHIELD, SLOT_WEAPON = 10, 25
# Baked light for the arms: soft ambient plus a key light from above, behind the camera
AMBIENT = np.array([0.55, 0.53, 0.5])
SUN = np.array([0.6, 0.58, 0.55])
SUN_DIR = np.array([0.15, -0.45, 0.88]) / np.linalg.norm([0.15, -0.45, 0.88])
TEXEL_BOOST = 2.5            # arms are closer to the eye than anything in the world
VFX_FPS = 20                 # samples a second of a spell visual's node animation (the engine's kVfxFps)


def fp_groups(nif):
    """Groups the first-person view plays (names <= 15 characters):
      Idle1h Idle2c Idle2w IdleHH IdleXbow IdleSpell, Walk*/Run* (1h 2c 2w HH),
      <W><Kind> wind-ups: start, min attack (loop start), max attack (loop stop), max attack (stop)
      <W><Kind>F the blow and the large follow-through: min hit, hit (loop), large follow stop;
      <W><Kind>FM / FS the medium / small follow-throughs
      <W>Eq / <W>Uneq, BowShoot(F) XbowShoot(F) ThrowShoot(F), Block, Hit1, KnockDown,
      CastSelf / CastTarget / CastTouch (release as loop start), SpellEq / SpellUneq,
      PickProbe / PickProbeEq / PickProbeUneq (a lockpick or probe)
    where W is 1h, 2c, 2w, HH, Bow, Xbow, Throw and Kind is Chop, Slash, Thrust."""
    keys = next((b["keys"] for b in nif.blocks if b["type"] == "NiTextKeyExtraData"), [])
    marks = {}
    for t, text in keys:
        for line in text.replace("\r", "").split("\n"):
            if ":" in line:
                g, what = (x.strip().lower() for x in line.split(":", 1))
                marks.setdefault((g, what), t)
    out = []

    def add(name, g, start, loop_start=None, loop_stop=None, stop=None):
        s, e = marks.get((g, start)), marks.get((g, stop or "stop"))
        if s is None or e is None or e < s:
            return
        ls = marks.get((g, loop_start), s) if loop_start else s
        le = marks.get((g, loop_stop), e) if loop_stop else e
        out.append({"name": name, "start": s, "loop_start": ls, "loop_stop": le, "stop": e})

    for name, g in [("Idle1h", "idle1h"), ("Idle2c", "idle2c"), ("Idle2w", "idle2w"), ("IdleHH", "idlehh"),
                    ("IdleXbow", "idlecrossbow"), ("IdleSpell", "idlespell")]:
        add(name, g, "start", "loop start", "loop stop")
    for sfx in ("1h", "2c", "2w", "hh"):
        tag = sfx.upper() if sfx == "hh" else sfx
        add("Walk" + tag, "walkforward" + sfx, "start", "loop start", "loop stop")
        add("Run" + tag, "runforward" + sfx, "start", "loop start", "loop stop")
    melee = [("1h", "weapononehand"), ("2c", "weapontwohand"), ("2w", "weapontwowide"), ("HH", "handtohand")]
    for tag, g in melee:
        for kind in ("chop", "slash", "thrust"):
            k = kind.capitalize()
            add(tag + k, g, f"{kind} start", f"{kind} min attack", f"{kind} max attack", f"{kind} max attack")
            # the blow and the large follow-through (hit mark as the loop), then the medium / small ones alone:
            # the runtime cuts over at the hit mark by the blow's strength (OpenMW)
            add(tag + k + "F", g, f"{kind} min hit", f"{kind} hit", f"{kind} hit", f"{kind} large follow stop")
            add(tag + k + "FM", g, f"{kind} medium follow start", stop=f"{kind} medium follow stop")
            add(tag + k + "FS", g, f"{kind} small follow start", stop=f"{kind} small follow stop")
    ranged = [("Bow", "bowandarrow"), ("Xbow", "crossbow"), ("Throw", "throwweapon")]
    for tag, g in melee + ranged + [("Spell", "spellcast")]:
        add(tag + "Eq", g, "equip start", stop="equip stop")
        add(tag + "Uneq", g, "unequip start", stop="unequip stop")
    for tag, g in ranged:
        add(tag + "Shoot", g, "shoot start", "shoot min attack", "shoot max attack", "shoot max attack")
        add(tag + "ShootF", g, "shoot release", stop="shoot follow stop")
    # (marks, no length: the arrow goes on the string at "shoot attach", the next bolt into the crossbow at
    # "shoot follow attach"; OpenMW's CharacterController::handleTextKey)
    add("BowAttach", "bowandarrow", "shoot attach", stop="shoot attach")
    add("XbowFAttach", "crossbow", "shoot follow attach", stop="shoot follow attach")
    # a lockpick or probe: its own use (a key's turn, no wind-up) and its own draw / put away (OpenMW's PickProbe)
    add("PickProbe", "pickprobe", "start")
    add("PickProbeEq", "pickprobe", "equip start", stop="equip stop")
    add("PickProbeUneq", "pickprobe", "unequip start", stop="unequip stop")
    add("Block", "shield", "block start", "block hit", "block hit", "block stop")
    add("Hit1", "hit1", "start")
    add("KnockDown", "knockdown", "start")
    for kind in ("self", "target", "touch"):
        add("Cast" + kind.capitalize(), "spellcast", f"{kind} start", f"{kind} release", f"{kind} release",
            f"{kind} stop")
    return out


def piece_name(model, slot):
    base = re.sub(r"[^a-z0-9_.]+", "_", model.lower().replace("\\", "/").split("/")[-1].rsplit(".nif", 1)[0])
    return f"{base}_{slot}"


class FirstPerson:
    def __init__(self, db, arch, textures, out_dir):
        self.db, self.arch, self.textures, self.out = db, arch, textures, out_dir
        self.nifs = {}
        self.pieces = {}          # name -> meshes (None while failed)
        self.fx_need = {}         # spell visual piece -> texels its particle-texture shapes want (vfx_model)
        self.vfx_len = {}         # spell visual piece -> its longest controller's stop time (vfx_model)
        objects = db["objects"]
        self.bodies = {r.id.lower(): r for r in objects.values() if r.tag == "BODY"}
        self.pose = {}

    def nif(self, path):
        key = path.lower()
        if key not in self.nifs:
            self.nifs[key] = Nif(self.arch.read(key)) if self.arch.exists(key) else None
        return self.nifs[key]

    def skeleton_pose(self, beast=False):
        path = SKELETONS[0]
        if path not in self.pose:
            nif = self.nif(path)
            self.pose[path] = skeleton_pose(nif, text_key_time(nif, "Idle1h: Start"))
        return self.pose[path]

    def body_model(self, part_id):
        """Model of a body part, preferring its first-person variant."""
        rec = self.bodies.get(part_id.lower() + ".1st") or self.bodies.get(part_id.lower())
        return rec.zstr("MODL") if rec is not None else None

    def tp_model(self, part_id):
        """Model of a body part as seen from outside (never the first-person variant)."""
        rec = self.bodies.get(part_id.lower())
        return rec.zstr("MODL") if rec is not None else None

    def third_person(self, objects):
        """The player seen from outside (third-person view, character creation's preview), built at run
        time on the NPC skeleton from these pieces (same format as the first-person ones):
          {"races": {race: {"m": {slot: piece}, "f": {...}}},          the bare body, every skin slot
           "heads": {race: {"m": [[body id, piece]...], "f": [...]}},   the heads to choose from
           "hairs": {race: {"m": [[body id, piece]...], "f": [...]}},
           "items": {item: {slot: [male piece, female piece]}}}         what armor / clothing puts on"""
        races, heads, hairs = {}, {}, {}
        for race in self.db["RACE"].values():
            rid = race.id.lower()
            beast = struct.unpack_from("<i", race.get("RADT"), 136)[0] & 2
            races[rid], heads[rid], hairs[rid] = {}, {}, {}
            for sex, female in (("m", 0), ("f", 1)):
                slots, hl, hr = {}, [], []
                for body in self.bodies.values():
                    part, vampire, flags, kind = body.get("BYDT")[:4]
                    if kind != 0 or vampire or (flags & 1) != female or body.id.lower().endswith(".1st"):
                        continue
                    if (body.zstr("FNAM") or "").lower() != rid:
                        continue
                    model = body.zstr("MODL")
                    if not model:
                        continue
                    if part in (0, 1):
                        p = self.piece(model, part, beast)
                        if p:
                            (hl if part == 0 else hr).append([body.id.lower(), p])
                        continue
                    for slot in SKIN_SLOTS.get(part, []):
                        if str(slot) not in slots:
                            p = self.piece(model, slot, beast)
                            if p:
                                slots[str(slot)] = p
                if female:
                    for k, p in races[rid]["m"].items():     # male parts fill the female gaps (as build())
                        slots.setdefault(k, p)
                races[rid][sex] = slots
                heads[rid][sex] = sorted(hl)
                hairs[rid][sex] = sorted(hr)
        items = {}
        for oid in objects:
            rec = self.db["objects"].get(oid)
            if rec is None or rec.tag not in ("ARMO", "CLOT"):
                continue
            slots = {}
            for slot, male, female in item_parts(rec):
                mm = self.tp_model(male) if male else None
                fm = self.tp_model(female) if female else None
                pm = self.piece(mm, slot) if mm else None
                pf = self.piece(fm, slot) if fm else pm
                if pm or pf:
                    slots[str(slot)] = [pm or "", pf or pm or ""]
            if slots:
                items[oid] = slots
        return {"races": races, "heads": heads, "hairs": hairs, "items": items}

    def mesh_record(self, s, bone, mirror):
        """Piece mesh from an extracted shape (same fields as actors.actor_mesh, bones by name)."""
        raw = s["raw"]
        tex = s["tex"]
        name = None
        if tex:
            # Texel density as for the world, but seen from arm's length instead of CLOSE_DISTANCE
            need = texels_needed(world_per_repeat(s["pos"], s["uv"], s["tris"])) * TEXEL_BOOST
            name = self.textures.request(tex, need, s["flags"] & 3)
        m = {"tex": name, "flags": s["flags"], "alpha_ref": s["alpha_ref"], "tris": np.asarray(s["tris"]),
             "uv": s["uv"] * [1.0, -1.0] + [0.0, 1.0], "kind": 0, "bone": ""}
        if s["skinned"]:
            m["kind"], m["pos"] = 1, raw["pos"]
            n = len(raw["pos"])
            palette, idx, w = [], np.zeros((n, 4), dtype=int), np.zeros((n, 4))
            for bname, skin_to_bone, vi, vw in raw["skin"]:
                pi = len(palette)
                palette.append((bname, [float(v) for v in np.asarray(skin_to_bone)[:3, :4].ravel()]))
                for v, weight in zip(vi, vw):
                    slot = int(np.argmin(w[v]))
                    if weight > w[v, slot]:
                        idx[v, slot], w[v, slot] = pi, weight
            total = np.maximum(w.sum(axis=1, keepdims=True), 1e-6)
            wq = np.round(w / total * 255).astype(int)
            wq[:, 0] += 255 - wq.sum(axis=1)
            m["palette"], m["vidx"], m["vw"] = palette, idx, np.clip(wq, 0, 255)
        else:
            local = (MIRROR_X if mirror else np.identity(4)) @ raw["node"]
            m["pos"] = raw["pos"] @ local[:3, :3].T + local[:3, 3]
            if mirror:
                m["tris"] = m["tris"].reshape(-1, 3)[:, [0, 2, 1]].ravel()
            m["bone"] = bone or ""
            # A bow's string: the shape morphs between key times the weapon's own timeline holds (the engine plays
            # it from the first-person group's time; the weights are those of NiGeomMorpherController)
            md = raw.get("morph")
            if md and md["relative"] and len(md["morphs"]) > 1:
                m["morph"] = [(mo["keys"], np.array(mo["vectors"]).reshape(-1, 3) @ local[:3, :3].T)
                              for mo in md["morphs"][1:]]
        # Lighting baked in skeleton space at the idle pose
        from convert_cell import bake_colors
        # (a world object's own piece, an animated activator: lit as the room it stands in, particles.auto)
        light = getattr(self, "light", None)
        m["colors"] = bake_colors(s, [], *light) if light else bake_colors(s, [], AMBIENT, SUN, SUN_DIR)
        return m

    def piece(self, model, slot, beast=False):
        """Converts one model for one slot (once); returns the piece name or None."""
        name = piece_name(model, slot)
        if name in self.pieces:
            return name if self.pieces[name] else None
        nif = self.nif("meshes\\" + model)
        if nif is None:
            self.pieces[name] = None
            return None
        bones = self.skeleton_pose(beast)
        bone_name = PART_BONES[slot] if slot < len(PART_BONES) else "Weapon Bone"
        bone = bones.get(bone_name.lower())
        mirror = bone_name.startswith("Left")
        meshes = []
        part_shapes = extract_shapes(self.arch, nif, skeleton=bones, along_emitter=slot == SLOT_WEAPON)
        # (a model with a skinned shape goes on as a skin: its rigid shapes dropped, as npc.py and OpenMW's attach)
        as_skin = slot != SLOT_WEAPON and any(s["skinned"] for s in part_shapes)
        for s in part_shapes:
            if "raw" not in s and slot == SLOT_WEAPON and not s["skinned"]:
                # a held weapon's spray (Hopesfire's glow, the flaming Trueflame's fire down the blade): its quads, in the
                # weapon's own space, ride the weapon bone with the blade
                s["raw"] = {"pos": np.asarray(s["pos"], dtype=float), "node": np.identity(4)}
            if "raw" not in s or (as_skin and not s["skinned"]):
                continue
            if s["skinned"]:
                sn = s["name"].lower()
                if slot != SLOT_WEAPON and not sn.startswith(("tri " + bone_name.lower(), "tril" + bone_name.lower())):
                    continue
            elif bone is not None:
                m = bone @ MIRROR_X if mirror else bone
                s["pos"] = s["pos"] @ m[:3, :3].T + m[:3, 3]
                s["nrm"] = s["nrm"] @ m[:3, :3].T
            meshes.append(self.mesh_record(s, bone_name.lower(), mirror))
        self.pieces[name] = meshes or None
        return name if meshes else None

    def item_model(self, model):
        """An item's own mesh at its root (arrows, bolts and thrown weapons in flight)."""
        name = piece_name(model, "item")
        if name in self.pieces:
            return name if self.pieces[name] else None
        nif = self.nif("meshes\\" + model)
        meshes = []
        if nif is not None:
            for s in extract_shapes(self.arch, nif):
                if "raw" in s and not s["skinned"]:
                    meshes.append(self.mesh_record(s, "", False))
        self.pieces[name] = meshes or None
        return name if meshes else None

    def ammo_piece(self, model):
        """An arrow or bolt as it sits in the bow or crossbow: its own mesh on the weapon bone, placed further by
        the weapon's ArrowBone (arrow_track) at run time."""
        name = piece_name(model, "ammo")
        if name in self.pieces:
            return name if self.pieces[name] else None
        nif = self.nif("meshes\\" + model)
        meshes = []
        if nif is not None:
            for s in extract_shapes(self.arch, nif):
                if "raw" in s and not s["skinned"]:
                    meshes.append(self.mesh_record(s, "weapon bone", False))
        self.pieces[name] = meshes or None
        return name if meshes else None

    def arrow_track(self, model):
        """A bow's or crossbow's "ArrowBone" in the weapon's own space, VFX_FPS times a second over its keyframes
        (it follows the string back): where OpenMW hangs the ammunition (NpcAnimation::getArrowBone; the skeletons
        have no "Bip01 Arrow"). Rows of 12 floats (3x4), or None."""
        nif = self.nif("meshes\\" + model)
        if nif is None:
            return None
        bone = next((b for b in nif.blocks if (b.get("name") or "").lower() == "arrowbone"), None)
        if bone is None:
            return None
        end = 0.0
        c = nif.get(bone.get("controller", -1))
        while c:
            if c["type"] == "NiKeyframeController" and nif.get(c["data"]):
                d = nif.get(c["data"])
                for k in ("rotations", "translations", "scales"):
                    if d.get(k):
                        end = max(end, d[k][-1][0])
            c = nif.get(c.get("next_controller", -1))
        rows = []
        for i in range(int(end * VFX_FPS) + 2):
            m = skeleton_pose(nif, i / VFX_FPS)["arrowbone"]
            rows.append([round(float(v), 3) for v in m[:3, :4].ravel()])
        return rows

    def vfx_model(self, model):
        """A spell visual's mesh at its root: its shapes, and its particle emitters as the crossed quads
        meshes.particle_shape makes (most casting / bolt / hit effects are particles only: without these
        they had no piece and a bolt flew as a plain square). A shape under a node with keyframes keeps its
        own space and carries its node's animation (vfx_tracks): an area burst's sphere grows from a speck,
        a cast's sparks run up the arms."""
        name = piece_name(model, "vfx")
        if name in self.pieces:
            return name if self.pieces[name] else None
        nif = self.nif("meshes\\" + model)
        meshes = []
        if nif is not None:
            tracks, billboard, emitters = vfx_tracks(nif)
            from creature import vis_keys
            parents = {c: b["index"] for b in nif.blocks for c in b.get("children", []) or []}
            # The root's first NiTexturingProperty: the shapes below sharing it take the magic effect's own particle
            # texture (OpenMW: nifloader marks them "overrideFx", Animation::addEffect overrideFirstRootTexture)
            root = nif.blocks[0] if nif.blocks else {}
            root_tex = next((p for p in root.get("properties") or [] if nif.get(p) and
                             nif.get(p)["type"] == "NiTexturingProperty"), None)
            # How long it plays: its longest controller's stop time, particle sprays' included (OpenMW's
            # FindMaxControllerLengthVisitor: a visual that doesn't loop goes when that time has passed)
            stops = [b["stop_time"] for b in nif.blocks if "stop_time" in b]
            if stops and max(stops) > 0:
                self.vfx_len[name] = round(float(max(stops)), 3)
            # (shapes hidden at rest that a NiVisController shows at times: Lightning's second strike; their
            # visibility goes into their fade track below)
            for s in extract_shapes(self.arch, nif, vis_hidden=True):
                if s["skinned"]:
                    continue
                track, rest = None, None
                widen, whiten = VFX_BOLD.get(model.lower(), (None, 0.0))
                if widen and "raw" in s:
                    s["pos"] = widen_rings(s["pos"], s["uv"], widen)
                    s["raw"]["pos"] = widen_rings(s["raw"]["pos"], s["uv"], widen)
                if "raw" in s:
                    track = tracks.get(s["block"])
                    rest = s["raw"]["node"]
                else:
                    s["particles"] = True
                    s["raw"] = {"pos": np.asarray(s["pos"], dtype=float), "node": np.identity(4)}   # particles: root space
                    e = emitter_of(s, emitters)
                    if e is not None and e in tracks:
                        # the quads ride their emitter, upright (they stand at its rest place)
                        rest = np.identity(4)
                        track = []
                        for wm in tracks[e]:
                            t = np.identity(4)
                            t[:3, 3] = wm[:3, 3] - emitters[e][:3, 3]
                            track.append(t)
                if track is not None:
                    # textures sized for the shape at its largest
                    peak = max(track, key=lambda m: abs(np.linalg.det(m[:3, :3])))
                    loc = np.linalg.inv(rest) if abs(np.linalg.det(rest[:3, :3])) > 1e-12 else np.identity(4)
                    s["pos"] = np.asarray(s["pos"]) @ (peak @ loc)[:3, :3].T + (peak @ loc)[:3, 3]
                m = self.mesh_record(s, "", False)
                if whiten and s.get("tex") and not s.get("particles"):
                    m["tex"] = self.textures.request_whitened(s["tex"], 64.0, s["flags"] & 3, whiten)
                m["particles"] = s.get("particles", False)
                if not m["particles"]:
                    m["uvtrack"], m["alphatrack"] = surface_tracks(nif, s)
                    vis = vis_keys(nif, s.get("block", -1), parents)
                    if vis:
                        # shown and hidden by NiVisControllers (Lightning's bolts flicking on and off): as its fade,
                        # nothing while hidden
                        n = max(len(m["alphatrack"]), int(self.vfx_len.get(name, 1.0) * VFX_FPS + 0.999) + 1)
                        fade = m["alphatrack"] + [m["alphatrack"][-1] if m["alphatrack"] else 1.0] * (n - len(m["alphatrack"]))
                        def shown(t):
                            on = vis[0][1]
                            for kt, v in vis:
                                if kt <= t + 1e-6:
                                    on = v
                            return on
                        m["alphatrack"] = [a if shown(k / VFX_FPS) else 0.0 for k, a in enumerate(fade)]
                    top = max(m["alphatrack"] or [1.0])
                    if top > 1.0:
                        # (a fade above the alpha it rests at: the vertices at its peak, the track a share of that)
                        c = np.asarray(m["colors"]).astype(float)
                        rest = (s.get("mat") or {}).get("alpha", 1.0)
                        if rest < 1e-3:
                            # (resting at alpha 0, Levitate's rings: nothing to scale, the peak itself)
                            c[:, 3] = np.minimum(255.0, 255.0 * top * 1e-3)
                        else:
                            c[:, 3] = np.minimum(255.0, c[:, 3] * top)
                        m["colors"] = c.round().astype(np.uint8)
                        m["alphatrack"] = [a / top for a in m["alphatrack"]]
                if root_tex is not None and s.get("texprop") == root_tex and s.get("block") != 0:
                    m["fx"] = True
                    self.fx_need[name] = max(self.fx_need.get(name, 0.0),
                                             texels_needed(world_per_repeat(s["pos"], s["uv"], s["tris"])) * TEXEL_BOOST)
                if track is not None:
                    m["pos"] = s["raw"]["pos"]                 # its own space: the track places it
                    m["track"] = [t[:3, :4] for t in track]
                    m["billboard"] = s.get("block") in billboard
                md = s["raw"].get("morph")
                if m.pop("morph", None) is not None and not m["particles"]:
                    # A shape that morphs (NiGeomMorpherController: Restoration's cast tube rising from a ring): its
                    # vertices baked VFX_FPS times a second up to the last weight key, the engine swapping in a
                    # frame's (path frames) and holding the last
                    rot = np.identity(3) if track is not None else s["raw"]["node"][:3, :3]
                    deltas = [(mo["keys"], np.array(mo["vectors"]).reshape(-1, 3) @ rot.T) for mo in md["morphs"][1:]]
                    end = max([k[-1][0] for k, _ in deltas if k] + [0.0])
                    frames = []
                    for k in range(int(end * VFX_FPS + 0.999) + 1):
                        pos = np.asarray(m["pos"], dtype=float).copy()
                        for keys, d in deltas:
                            if keys:
                                pos = pos + float(np.ravel(sample(keys, k / VFX_FPS))[0]) * d
                        frames.append((pos, np.asarray(m["colors"])))
                    m["path"] = (float(VFX_FPS), frames)
                meshes.append(m)
        self.pieces[name] = meshes or None
        return name if meshes else None

    def build(self, objects):
        """game.json "firstperson" for the level's items (objects: game.json objects)."""
        races = {}
        for race in self.db["RACE"].values():
            rid = race.id.lower()
            beast = struct.unpack_from("<i", race.get("RADT"), 136)[0] & 2
            entry = {}
            for sex, female in (("m", 0), ("f", 1)):
                slots = {}
                for body in self.bodies.values():
                    part, vampire, flags, kind = body.get("BYDT")[:4]
                    if kind != 0 or vampire or (flags & 1) != female or body.id.lower().endswith(".1st"):
                        continue
                    if (body.zstr("FNAM") or "").lower() != rid:
                        continue
                    for slot in SKIN_SLOTS.get(part, []):
                        if slot in ARM_SLOTS and str(slot) not in slots:
                            model = self.body_model(body.id)
                            p = self.piece(model, slot, beast) if model else None
                            if p:
                                slots[str(slot)] = p
                if female:
                    # Male parts stand in for missing female ones, as in OpenMW (getBodyParts): the
                    # female Argonian has no forearm, which left a gap between wrist and upper arm
                    for k, p in entry["m"].items():
                        slots.setdefault(k, p)
                entry[sex] = slots
            races[rid] = entry
        items = {}
        for oid, o in objects.items():
            rec = self.db["objects"].get(oid)
            if rec is None or rec.tag not in ITEM_TAGS:
                continue
            e = {}
            model = rec.zstr("MODL")
            # Every item's own mesh: arrows in flight, and anything the player drops
            if model and not model.lower().replace("/", "\\").rsplit("\\", 1)[-1].startswith("marker"):
                m = self.item_model(model)
                if m:
                    e["model"] = m
            if rec.tag not in ("WEAP", "LOCK", "PROB", "ARMO", "CLOT"):
                if e:
                    items[oid] = e
                continue
            # A lockpick or probe is held on the weapon bone as a weapon is
            if rec.tag in ("WEAP", "LOCK", "PROB") and model:
                p = self.piece(model, SLOT_WEAPON)
                if p:
                    e["slots"] = {str(SLOT_WEAPON): [p, p]}
                # Bows and crossbows: where the arrow or bolt sits; arrows and bolts: their piece to sit there
                wtype = struct.unpack_from("<h", rec.get("WPDT"), 8)[0] if rec.tag == "WEAP" and rec.get("WPDT") else -1
                if wtype in (9, 10):
                    track = self.arrow_track(model)
                    if track:
                        e["arrow"] = track
                elif wtype in (12, 13):
                    p = self.ammo_piece(model)
                    if p:
                        e["ammo"] = p
            else:
                slots = {}
                for slot, male, female in item_parts(rec):
                    # Any part with a first-person (".1st") model is drawn in first person, whatever its slot: a
                    # robe's sleeves are its chest part's (OpenMW's addPartGroup); others only on the arms
                    if slot not in ARM_SLOTS and not any(p and p.lower() + ".1st" in self.bodies for p in (male, female)):
                        continue
                    pm = self.piece(self.body_model(male), slot) if male and self.body_model(male) else None
                    pf = self.piece(self.body_model(female), slot) if female and self.body_model(female) else pm
                    slots[str(slot)] = [pm or "", pf or pm or ""]
                if slots:
                    e["slots"] = slots
            if e:
                items[oid] = e
        # Spell visuals: every static a magic effect names, as a piece (drawn where spells are cast and hit), and
        # the ones OpenMW falls back on for an effect that names none (VFX_DefaultBolt: Shock Damage's bolt)
        vfx = {}
        names = [(r.zstr(t) or "").lower() for r in read_records_mgef(self.db) for t in ("CVFX", "BVFX", "HVFX", "AVFX")]
        names += ["vfx_defaultcast", "vfx_defaultbolt", "vfx_defaulthit", "vfx_defaultarea"]
        names += ["vfx_summon_start", "vfx_summon_end"]      # a summoned creature arriving and leaving (OpenMW)
        names += ["vfx_hands"]          # the glow on both hands while casting (OpenMW's CharacterController)
        for sid in names:
            rec = self.db["objects"].get(sid)
            if sid and sid not in vfx and rec is not None and rec.zstr("MODL"):
                p = self.vfx_model(rec.zstr("MODL"))
                if p:
                    vfx[sid] = p
        vfx.update(self.blood())
        # Each magic effect's particle texture (MGEF PTEX), for the visuals with shapes that take it (fx_need): a Drain
        # bolt flies as Destruction's bolt in its own vfx_map21 rather than the fire one
        ptex = {}
        for r in read_records_mgef(self.db):
            tex = (r.zstr("PTEX") or "").lower()
            # (an effect with no bolt of its own flies the default one, as the engine does: Disintegrate Armor's fire)
            pieces = [vfx.get((r.zstr(t) or ("vfx_defaultbolt" if t == "BVFX" else "")).lower())
                      for t in ("CVFX", "BVFX", "HVFX", "AVFX")]
            need = max([self.fx_need.get(p, 0.0) for p in pieces if p] or [0.0])
            if not tex or need <= 0:
                continue
            path = "textures\\" + tex.replace("/", "\\")
            dds = re.sub(r"\.[^.\\]+$", ".dds", path)
            path = next((c for c in (dds, path) if self.arch.exists(c)), None)
            if path:
                ptex[str(struct.unpack("<i", r.get("INDX"))[0])] = self.textures.request(path, need, True)
        vfxlen = {p: self.vfx_len[p] for p in set(vfx.values()) if p in self.vfx_len}
        return {"races": races, "items": items, "third": self.third_person(objects), "vfx": vfx, "ptex": ptex,
                "vfxlen": vfxlen}

    def blood(self):
        """Morrowind.ini [Blood]: the splat a weapon hit throws (Blood Model 0..2: BloodSplat, BloodSplat2, BloodSplat3,
        particle bursts; OpenMW picks one at random), in each blood's texture: 0 red, 1 skeleton white, 2 metal sparks
        gold. Pieces "blood<texture>" (the first model) and "blood<texture>_<model>"."""
        out = {}
        for m, model in enumerate(("bloodsplat", "bloodsplat2", "bloodsplat3")):
            nif = self.nif("meshes\\" + model + ".nif")
            if nif is None:
                continue
            for k, tex in enumerate(("textures\\tx_blood.dds", "textures\\tx_blood_white.dds", "textures\\tx_blood_gold.dds")):
                if not self.arch.exists(tex):
                    continue
                meshes = []
                for s in extract_shapes(self.arch, nif):
                    if s["skinned"]:
                        continue
                    if "raw" not in s:
                        s["raw"] = {"pos": np.asarray(s["pos"], dtype=float), "node": np.identity(4)}   # particles: root space
                    s["tex"] = tex
                    meshes.append(self.mesh_record(s, "", False))
                if meshes:
                    name = f"blood{k}" if m == 0 else f"blood{k}_{m}"
                    self.pieces[name] = meshes
                    out[name] = name
        return out

    def write(self):
        self.out.mkdir(parents=True, exist_ok=True)
        with open(self.out / "skeletons.skl", "wb") as f:
            f.write(b"MWS1" + struct.pack("<I", len(SKELETONS)))
            write_skeleton_list(f, [skeleton_export(self.nif(p), fp_groups(self.nif(p))) for p in SKELETONS])
        written = {"skeletons.skl"}
        for name, meshes in self.pieces.items():
            if not meshes:
                continue
            written.add(name + ".fpm")
            write_piece(self.out / (name + ".fpm"), meshes)
        for p in self.out.glob("*"):
            if p.name not in written:
                p.unlink()
        return sum(p.stat().st_size for p in self.out.glob("*"))


def xyz_matrix(x, y, z):
    """NiKeyframeData's XYZ rotation keys: about X, then Y, then Z."""
    cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return rz @ ry @ rx


# Spell visuals drawn bolder on the 3DS (model: (times as wide, share of the way to white)): Lightning's bolts are
# tubes 1.4 units across with a white core in a purple edge, a pixel or two on its screen, where OpenMW shows a white
# core two or three wide (user's call: wider, whiter centre)
VFX_BOLD = {r"e\lightningbolts.nif": (2.5, 0.5)}


def widen_rings(pos, uv, factor):
    """A tube's rings (the vertices sharing a texture v) spread from their own middle: thicker, its path kept."""
    pos = np.asarray(pos, dtype=float).copy()
    keys = np.round(np.asarray(uv)[:, 1], 3)
    for k in np.unique(keys):
        ring = keys == k
        mid = pos[ring].mean(axis=0)
        pos[ring] = mid + (pos[ring] - mid) * factor
    return pos


def controller_time(c, t):
    """The time a controller plays at t seconds into the model (OpenMW's ControllerFunction): frequency and phase, then
    inside its start .. stop as its flags say (bits 1-2: 0 round again, 1 back and forth, 2 hold at the ends)."""
    lo, hi = c["start_time"], c["stop_time"]
    x = c.get("frequency", 1.0) * t + c.get("phase", 0.0)
    if lo <= x <= hi:
        return x
    mode = (c.get("ctrl_flags", 0) >> 1) & 3
    span = hi - lo
    if mode == 0:
        if span <= 0:
            return lo
        f = (x - lo) / span
        return lo + (f - math.floor(f)) * span
    if mode == 1:
        if span <= 0:
            return lo
        f = (x - lo) / span
        r = (f - math.floor(f)) * span
        return lo + r if int(math.floor(f)) % 2 == 0 else hi - r
    return min(max(x, lo), hi)


def vfx_tracks(nif):
    """A spell visual's node keyframes, sampled VFX_FPS times a second from 0 to the last key.
    Returns ({block: [4x4 root-space matrix per sample]} for every object under an animated node,
    {blocks under a NiBillboardNode}, {particle emitter block: its rest root-space matrix}).
    Billboards turn to face the eye (OpenMW's BillboardCallback): the engine keeps the track's place and size."""
    keyframes, timing = {}, {}
    for i, b in enumerate(nif.blocks):
        c = b.get("controller", -1)
        while c is not None and c >= 0:
            cb = nif.get(c)
            if cb is None:
                break
            if cb["type"] == "NiKeyframeController" and nif.get(cb.get("data", -1)):
                keyframes[i] = nif.get(cb["data"])
                timing[i] = cb
            c = cb.get("next_controller", -1)
    length = 0.0
    for kd in keyframes.values():
        for keys in [kd["translations"], kd["scales"], kd["rotations"]] + (kd["xyz_rotations"] or []):
            if keys:
                length = max(length, keys[-1][0])
    # (a keyframe controller that goes round plays as long as the model: its keys may sit anywhere, even before 0)
    if any((cb.get("ctrl_flags", 0) >> 1) & 3 != 2 for cb in timing.values()):
        length = max([length] + [b["stop_time"] for b in nif.blocks if "stop_time" in b and abs(b["stop_time"]) < 1e6])

    def local_at(b, idx, t):
        kd = keyframes.get(idx)
        if kd:
            t = controller_time(timing[idx], t)
        rot = np.array(b["rotation"]).reshape(3, 3)
        trans, scale = np.array(b["translation"]), b["scale"]
        if kd:
            if kd["rotations"]:
                rot = quat_matrix(sample(kd["rotations"], t, quat=True))
            elif kd["xyz_rotations"]:
                rot = xyz_matrix(*[sample(k, t)[0] if k else 0.0 for k in kd["xyz_rotations"]])
            if kd["translations"]:
                trans = sample(kd["translations"], t)
            if kd["scales"]:
                scale = sample(kd["scales"], t)[0]
        m = np.identity(4)
        m[:3, :3] = rot * scale
        m[:3, 3] = trans
        return m

    tracks, billboard, emitters = {}, set(), {}

    def walk(idx, parent, t, animated, board, out):
        b = nif.get(idx)
        if b is None or "translation" not in b or b["type"] == "RootCollisionNode" or b.get("flags", 0) & 1:
            return
        animated = animated or idx in keyframes
        board = board or b["type"] == "NiBillboardNode"
        m = parent @ local_at(b, idx, t)
        if animated:
            out[idx] = m
        if board:
            billboard.add(idx)
        if b["type"] in ("NiRotatingParticles", "NiAutoNormalParticles"):
            ctrl = nif.get(b.get("controller", -1))
            if ctrl and ctrl.get("emitter", -1) >= 0:
                emitters[idx] = ctrl["emitter"]
        for c in b.get("children", []):
            walk(c, m, t, animated, board, out)

    if not keyframes or length <= 0.0:
        return {}, set(), {}
    for k in range(int(length * VFX_FPS + 0.999) + 1):
        out = {}
        for r in nif.roots:
            walk(r, np.identity(4), min(k / VFX_FPS, length), False, False, out)
        for idx, m in out.items():
            tracks.setdefault(idx, []).append(m)
    # particles: their emitter node's rest matrix (as particle_shape placed the quads: no keyframes)
    still = {}
    walk_nif(nif, lambda b, m, p: still.setdefault(b["index"], m))
    emitters = {e: still[e] for e in set(emitters.values()) if e in still}
    return tracks, billboard, emitters


def surface_tracks(nif, s):
    """A spell visual shape's texture slide and fade, VFX_FPS times a second from 0 to the controller's stop:
    [(u, v)] from its NiUVController (the offset keys; tiling left out) and [alpha] from the NiAlphaController on its
    NiMaterialProperty, as a share of the alpha it rests at (OpenMW's UVController, AlphaController). [] for none."""
    def chain(block):
        c = nif.get(block.get("controller", -1)) if block else None
        while c:
            yield c
            c = nif.get(c.get("next_controller", -1))

    shape = nif.get(s.get("block", -1))
    uv, alpha = [], []
    for c in chain(shape):
        d = nif.get(c.get("data", -1))
        if c["type"] == "NiUVController" and d and (d["groups"][0] or d["groups"][1]):
            n = int(c["stop_time"] * VFX_FPS + 0.999) + 1
            ts = [controller_time(c, k / VFX_FPS) for k in range(n)]
            uv = [(sample(d["groups"][0], t)[0] if d["groups"][0] else 0.0,
                   sample(d["groups"][1], t)[0] if d["groups"][1] else 0.0) for t in ts]
    mat = s.get("mat")
    for c in chain(mat):
        d = nif.get(c.get("data", -1))
        if c["type"] == "NiAlphaController" and d and d.get("keys"):
            rest = max(1e-3, mat.get("alpha", 1.0))
            n = int(c["stop_time"] * VFX_FPS + 0.999) + 1
            alpha = [max(0.0, sample(d["keys"], controller_time(c, k / VFX_FPS))[0] / rest) for k in range(n)]
    return uv, alpha


def emitter_of(s, emitters):
    """The emitter a particle shape's quads stand on: particle_shape centres them on its rest place."""
    c = np.asarray(s["pos"]).mean(axis=0)
    best, bd = None, 0.5
    for e, m in emitters.items():
        d = np.hypot(c[0] - m[0, 3], c[1] - m[1, 3])
        if d < bd:
            best, bd = e, d
    return best


def read_records_mgef(db):
    from mwfiles import read_game
    return read_game({"MGEF"})


def write_piece(path, meshes):
    texs = sorted({m["tex"] for m in meshes if m["tex"]})
    with open(path, "wb") as f:
        f.write(b"MWP1" + struct.pack("<I", len(texs)))
        for t in texs:
            f.write(t.encode("ascii").ljust(64, b"\x00"))
        f.write(struct.pack("<I", len(meshes)))
        for m in meshes:
            n = len(m["pos"])
            tris = np.asarray(m["tris"]).ravel()
            more = (1 if m.get("morph") else 0) | (2 if m.get("track") else 0) | (4 if m.get("billboard") else 0)                 | (8 if m.get("fx") else 0) | (32 if m.get("flip") else 0) | (64 if m.get("path") else 0)                 | (128 if m.get("sprites") or m.get("uvtrack") or m.get("alphatrack") else 0)
            f.write(struct.pack("<iBBBB", texs.index(m["tex"]) if m["tex"] else -1, m["flags"], m["alpha_ref"],
                                m["kind"], more))
            f.write(m["bone"].encode("latin-1")[:31].ljust(32, b"\x00"))
            f.write(struct.pack("<II", n, len(tris)))
            v = np.zeros(n, dtype=[("pos", "<f4", 3), ("uv", "<f4", 2), ("color", "u1", 4)])
            v["pos"], v["uv"], v["color"] = m["pos"], m["uv"], m["colors"]
            f.write(v.tobytes())
            f.write(tris.astype("<u2").tobytes())
            if len(tris) & 1:
                f.write(b"\x00\x00")
            if m["kind"] == 1:
                f.write(struct.pack("<I", len(m["palette"])))
                for bname, mat in m["palette"]:
                    f.write(bname.encode("latin-1")[:31].ljust(32, b"\x00"))
                    f.write(struct.pack("<12f", *mat))
                f.write(np.concatenate([m["vidx"], m["vw"]], axis=1).astype(np.uint8).tobytes())
            if m.get("morph"):
                # per morph: u32 n, n x (f32 t, f32 weight), f32 delta[num_vertices * 3]
                f.write(struct.pack("<I", len(m["morph"])))
                for keys, delta in m["morph"]:
                    f.write(struct.pack("<I", len(keys)))
                    for t, wt in keys:
                        f.write(struct.pack("<ff", t, wt))
                    f.write(np.asarray(delta, dtype="<f4").tobytes())
            if m.get("track"):
                f.write(struct.pack("<I", len(m["track"])))
                f.write(np.asarray(m["track"], dtype="<f4").tobytes())
            if m.get("flip"):
                # a flipbook (particles.py): u16 frames, u16 columns, u16 rows, f32 frames a second; frame 0 the
                # top-left cell of the texture, its UVs the mesh's
                frames, cols, rows, fps = m["flip"]
                f.write(struct.pack("<HHHf", frames, cols, rows, fps))
            if m.get("path"):
                # baked particle paths (particles.py): u16 frames, f32 frames a second, then per frame per vertex
                # f32 pos[3], u8 rgba[4]
                fps, frames = m["path"]
                f.write(struct.pack("<Hf", len(frames), fps))
                for pos, rgba in frames:
                    v = np.zeros(n, dtype=[("pos", "<f4", 3), ("color", "u1", 4)])
                    v["pos"], v["color"] = pos, rgba
                    f.write(v.tobytes())
            if more & 128:
                # u8: 1 particle records follow, 2 a texture slide, 4 a fade, 8 the records repeat
                f.write(struct.pack("<B", (1 if m.get("sprites") else 0) | (2 if m.get("uvtrack") else 0)
                                    | (4 if m.get("alphatrack") else 0)
                                    | (8 if m.get("sprites") and m["sprites"].get("repeat") else 0)))
            if m.get("uvtrack"):
                # its texture's offset, VFX_FPS a second: u32 n, n x f32 (u, v) (the file's own v, down)
                f.write(struct.pack("<I", len(m["uvtrack"])))
                f.write(np.asarray(m["uvtrack"], dtype="<f4").tobytes())
            if m.get("alphatrack"):
                # its alpha as a share of its own, VFX_FPS a second: u32 n, n x f32
                f.write(struct.pack("<I", len(m["alphatrack"])))
                f.write(np.asarray(m["alphatrack"], dtype="<f4").tobytes())
            if m.get("sprites"):
                # particle records (particles.py): f32 accel[3], f32 grow, f32 fade, u8 keys, per key f32 t (of a
                # life) + u8 rgba[4], u32 count, per particle f32 born, pos[3], vel[3], life, size (piece space)
                sp = m["sprites"]
                f.write(struct.pack("<3fffB", *sp["accel"], sp["grow"], sp["fade"], len(sp["keys"])))
                for t, rgba in sp["keys"]:
                    f.write(struct.pack("<f4B", t, *rgba))
                f.write(struct.pack("<I", len(sp["records"])))
                f.write(np.asarray(sp["records"], dtype="<f4").tobytes())
                if sp.get("repeat"):
                    # shed again every period seconds from start until stop (a creature's long fire): f32 x 3
                    f.write(struct.pack("<3f", *sp["repeat"]))
