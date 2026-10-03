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
                    kind 1: u32 palette size, per entry: char bone[32], f32 skin_to_bone[12];
                            per vertex u8 index[4], u8 weight[4]
Rigid positions are in their bone's space, skinned ones in skin space; bones are named, so a
piece fits either skeleton. game.json "firstperson" says which piece goes where:
  {"races": {race: {"m": {slot: piece}, "f": {...}}},
   "items": {item: {"slots": {slot: [male piece, female piece]}, "model": piece}}}
Slots are body part slots (npc.PART_BONES): 6/7 hands, 8/9 wrists, 10 shield, 11/12 forearms,
13/14 upper arms, 25 weapon. "model" is an item's own mesh at its root (arrows in flight, dropped items).
"""
import re
import struct

import numpy as np

from actors import skeleton_export, write_skeleton_list
from meshes import extract_shapes
from npc import MIRROR_X, PART_BONES, SKIN_SLOTS, item_parts, skeleton_pose, text_key_time
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


def fp_groups(nif):
    """Groups the first-person view plays (names <= 15 characters):
      Idle1h Idle2c Idle2w IdleHH IdleXbow IdleSpell, Walk*/Run* (1h 2c 2w HH),
      <W><Kind> wind-ups: start, min attack (loop start), max attack (loop stop), max attack (stop)
      <W><Kind>F the blow and the large follow-through: min hit, hit (loop), large follow stop;
      <W><Kind>FM / FS the medium / small follow-throughs
      <W>Eq / <W>Uneq, BowShoot(F) XbowShoot(F) ThrowShoot(F), Block, Hit1, KnockDown,
      CastSelf / CastTarget / CastTouch (release as loop start), SpellEq / SpellUneq
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
        # Lighting baked in skeleton space at the idle pose
        from convert_cell import bake_colors
        m["colors"] = bake_colors(s, [], AMBIENT, SUN, SUN_DIR)
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
        for s in extract_shapes(self.arch, nif, skeleton=bones):
            if "raw" not in s:
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

    def vfx_model(self, model):
        """A spell visual's mesh at its root: its shapes, and its particle emitters as the crossed quads
        meshes.particle_shape makes (most casting / bolt / hit effects are particles only: without these
        they had no piece and a bolt flew as a plain square)."""
        name = piece_name(model, "vfx")
        if name in self.pieces:
            return name if self.pieces[name] else None
        nif = self.nif("meshes\\" + model)
        meshes = []
        if nif is not None:
            for s in extract_shapes(self.arch, nif):
                if s["skinned"]:
                    continue
                if "raw" not in s:
                    s["raw"] = {"pos": np.asarray(s["pos"], dtype=float), "node": np.identity(4)}   # particles: root space
                meshes.append(self.mesh_record(s, "", False))
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
            if rec.tag not in ("WEAP", "ARMO", "CLOT"):
                if e:
                    items[oid] = e
                continue
            if rec.tag == "WEAP" and model:
                p = self.piece(model, SLOT_WEAPON)
                if p:
                    e["slots"] = {str(SLOT_WEAPON): [p, p]}
            else:
                slots = {}
                for slot, male, female in item_parts(rec):
                    if slot not in ARM_SLOTS:
                        continue
                    pm = self.piece(self.body_model(male), slot) if male and self.body_model(male) else None
                    pf = self.piece(self.body_model(female), slot) if female and self.body_model(female) else pm
                    slots[str(slot)] = [pm or "", pf or pm or ""]
                if slots:
                    e["slots"] = slots
            if e:
                items[oid] = e
        # Spell visuals: every static a magic effect names, as a piece (drawn where spells are cast and hit)
        vfx = {}
        for r in read_records_mgef(self.db):
            for t in ("CVFX", "BVFX", "HVFX", "AVFX"):
                sid = (r.zstr(t) or "").lower()
                rec = self.db["objects"].get(sid)
                if sid and sid not in vfx and rec is not None and rec.zstr("MODL"):
                    p = self.vfx_model(rec.zstr("MODL"))
                    if p:
                        vfx[sid] = p
        vfx.update(self.blood())
        return {"races": races, "items": items, "third": self.third_person(objects), "vfx": vfx}

    def blood(self):
        """Morrowind.ini [Blood]: the splat a weapon hit throws (BloodSplat.nif, a particle burst), in each blood's
        texture: 0 red, 1 skeleton white, 2 metal sparks gold. Pieces "blood0".."blood2"."""
        out = {}
        nif = self.nif("meshes\\bloodsplat.nif")
        if nif is None:
            return out
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
                self.pieces[f"blood{k}"] = meshes
                out[f"blood{k}"] = f"blood{k}"
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


def read_records_mgef(db):
    from mwfiles import DATA_FILES, read_records
    return read_records(DATA_FILES / "Morrowind.esm", {"MGEF"})


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
            f.write(struct.pack("<iBBBB", texs.index(m["tex"]) if m["tex"] else -1, m["flags"], m["alpha_ref"],
                                m["kind"], 0))
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
