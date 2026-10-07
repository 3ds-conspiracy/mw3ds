"""Builds data/game.json: everything the 3DS needs besides geometry.

  objects    records referenced by the level (items, doors, containers, NPCs ...)
  scripts    compiled local/global scripts (see mwscript.py for the tree format)
  dialogue   topics with responses pre-filtered for the level's NPCs (static filters
             resolved here, dynamic ones kept as conditions), result scripts compiled
  journal    quest entries for every journal the level can touch
  classes / races / birthsigns / skills / globals / gmst
  sounds     sound id -> converted file, plus voice files used by Say / responses
"""
import re
import math
import struct

from mwfiles import DATA_FILES, read_records
from mwscript import ScriptError, compile_script, compile_snippet
from creature import creature_stats
from npcstats import bind, npc_items, npc_stats, weapon_damage

ITEM_TYPES = {"MISC", "BOOK", "ALCH", "INGR", "WEAP", "ARMO", "CLOT", "LIGH", "APPA", "LOCK", "PROB", "REPA"}
# Engine sounds used by the runtime (item pickup, doors, UI, journal, footsteps)
ENGINE_SOUNDS = [
    "Item Misc Up", "Item Misc Down", "Item Book Up", "Item Gold Up", "Item Potion Up", "Item Ingredient Up",
    "Item Weapon Shortblade Up", "Item Clothes Up", "Item Lockpick Up", "Item Probe Up", "Item Repair Up",
    "Item Apparatus Up", "Item Armor Light Up", "Item Armor Medium Up", "Item Armor Heavy Up",
    "Menu Click", "Menu Size", "Book Open", "Book Close", "Book Page", "scroll", "Open Lock", "LockedDoor",
    "LockedChest", "Disarm Trap", "Disarm Trap Fail", "Open Lock Fail", "FootBareLeft", "FootBareRight", "chest open", "chest close", "Door Heavy Open",
    "Door Heavy Close", "Door Latched One Open", "Door Latched One Close", "Door Stone Open",
    # combat
    "SwishS", "SwishM", "SwishL", "Weapon Swish", "Health Damage", "Hand To Hand Hit", "Hand To Hand Hit 2",
    "Light Armor Hit", "Medium Armor Hit", "Heavy Armor Hit", "Body Fall Medium", "Item Gold Down",
    "skillraise", "levelUP", "Drink", "Swallow",
    "alteration cast", "conjuration cast", "destruction cast", "illusion cast", "mysticism cast", "restoration cast",
    "alteration hit", "conjuration hit", "destruction hit", "illusion hit", "mysticism hit", "restoration hit",
    "Spell Failure Destruction", "spellmake success",
    "bowPull", "bowShoot", "crossbowPull", "crossbowShoot", "Item Weapon Blunt Up", "Item Weapon Bow Up",
    "Item Weapon Longblade Up", "Item Weapon Spear Up", "Item Weapon Crossbow Up", "DefaultLand",
    "destruction bolt", "repair", "repair fail",
]


# A game started past character creation gets the first of these that exists, and gold
STARTER_WEAPONS = ["iron dagger", "chitin dagger", "iron shortsword", "iron_shortsword", "chitin short sword"]
STARTER_GOLD = 100


def pick_water_sound(db):
    """Sound id for the shore ambience: the first SOUN whose file looks like waves / lapping water."""
    skip = ("underwater", "drip", "splash", "swim", "fall", "boil", "bubbl")
    for word in ("wave", "lap", "ocean", "sea", "water"):
        for sid, rec in sorted(db["SOUN"].items()):
            f = (rec.zstr("FNAM") or "").lower()
            if word in f.rsplit("\\", 1)[-1] and not any(s in f for s in skip):
                return sid
    return None


def resolve_leveled_item(db, lid, depth=0):
    """Item record for a leveled item list id: its lowest-level entry, following nested lists."""
    entries = db["LEVI"].get(lid.lower())
    if not entries or depth > 4:
        return None
    for _, iid in sorted(entries):
        rec = db["objects"].get(iid.lower())
        if rec is not None:
            return rec
        nested = resolve_leveled_item(db, iid, depth + 1)
        if nested is not None:
            return nested
    return None


def z(d):
    return d.split(b"\0", 1)[0].decode("latin-1") if d is not None else None


def txt(s):
    """Record strings are read as latin-1; display text is really Windows-1252 (curly quotes etc.)."""
    if not s:
        return ""
    try:
        return s.encode("latin-1").decode("cp1252", errors="replace")
    except UnicodeEncodeError:
        return s


def book_text(s):
    """Morrowind book markup (HTML-ish) -> plain text with newlines."""
    s = txt(s)
    # pictures stay, as markers the book screen draws: [[img:bookart\x.dds]]
    s = re.sub(r'(?i)<img[^>]*src\s*=\s*"?([^" >]+)"?[^>]*>',
               lambda m: "\n[[img:" + m.group(1).lower().replace("/", "\\") + "]]\n", s)
    s = re.sub(r"(?i)<br\s*/?>", "\n", s)
    s = re.sub(r"(?i)<p[^>]*>", "\n\n", s)
    # <DIV ALIGN="CENTER"> ...: those lines start with \x01 (the book screen centres them)
    out, center = [], False
    for piece in re.split(r"(?i)(<div[^>]*>|</div>)", s):
        if re.match(r"(?i)<div", piece):
            center = bool(re.search(r'(?i)align\s*=\s*"?center', piece))
            out.append("\n")
            continue
        if re.match(r"(?i)</div>", piece):
            out.append("\n")
            continue
        piece = re.sub(r"<[^>]*>", "", piece).replace("\r", "")
        if center:
            piece = "\n".join("\x01" + l.strip() if l.strip() and not l.strip().startswith("[[img:") else l
                              for l in piece.split("\n"))
        out.append(piece)
    s = "".join(out)
    s = re.sub(r"[ \t]+\n", "\n", s)
    return re.sub(r"\n{3,}", "\n\n", s).strip()


def i32(d, off=0):
    return struct.unpack_from("<i", d, off)[0]


def f32(d, off=0):
    return struct.unpack_from("<f", d, off)[0]


# ---------------------------------------------------------------- actors

BOUND_ITEMS = ["bound_dagger", "bound_longsword", "bound_mace", "bound_battle_axe", "bound_spear", "bound_longbow",
               "bound_cuirass", "bound_helm", "bound_boots", "bound_shield", "bound_gauntlet_left", "bound_gauntlet_right"]


def normal_weapon_resistance(rec, db):
    """Resist Normal Weapons less Weakness to Normal Weapons from an actor's abilities (ghosts, daedra)."""
    total = 0
    for t, d in rec.subs:
        if t != "NPCS":
            continue
        sp = db["SPEL"].get(z(d).lower())
        if sp is None or i32(sp.get("SPDT")) != 1:
            continue
        for e in enam_effects(sp):
            total += e["min"] if e["effect"] == 98 else -e["min"] if e["effect"] == 36 else 0
    return total


# Effects an actor's abilities, diseases and curses keep on it for good that matter when it is the target or the
# defender (OpenMW reads them from its active spells): resistances and weaknesses, the elemental shields, Shield,
# Sanctuary, Chameleon, Invisibility, Reflect, Spell Absorption, Fortify Attack, Blind
CONSTANT_TARGET_EFFECTS = set(range(28, 37)) | set(range(90, 100)) | {3, 4, 5, 6, 39, 40, 42, 47, 67, 68, 117}


def constant_effects(rec, db):
    """[[effect, magnitude], ...] summed over the actor's own abilities / diseases / curses (NPCS) and, for an NPC, its
    race's abilities (the race record's NPCS)."""
    ids = [z(d).lower() for t, d in rec.subs if t == "NPCS"]
    if rec.tag == "NPC_":
        race = db["RACE"].get((rec.zstr("RNAM") or "").lower())
        if race is not None:
            ids += [z(d).lower() for t, d in race.subs if t == "NPCS"]
    total = {}
    for sid in ids:
        sp = db["SPEL"].get(sid)
        if sp is None or i32(sp.get("SPDT")) not in (1, 2, 3, 4):
            continue
        for e in enam_effects(sp):
            if e["effect"] in CONSTANT_TARGET_EFFECTS:
                total[e["effect"]] = total.get(e["effect"], 0) + e["min"]
    return [[k, v] for k, v in sorted(total.items()) if v]


def npc_info(rec, db):
    """An actor's game.json entry (with its AI Hello: how near it greets, AIDT byte 0)."""
    info = _npc_info(rec, db)
    effects = constant_effects(rec, db)
    if effects:
        info["const_effects"] = effects
    aidt = rec.get("AIDT")
    info["hello"] = aidt[0] if aidt else 30
    info["alarm"] = aidt[4] if aidt and len(aidt) > 4 else 0
    flag = rec.get("FLAG")
    flags = struct.unpack("<I", flag[:4])[0] if flag and len(flag) >= 4 else 0
    # NPC_: 0x2 essential, 0x4 respawns; CREA: 0x2 respawns, 0x80 essential
    info["respawn"] = 1 if flags & (0x2 if rec.tag == "CREA" else 0x4) else 0
    info["essential"] = 1 if flags & (0x80 if rec.tag == "CREA" else 0x2) else 0
    if rec.tag == "CREA" and flags & 0x30:
        info["afloat"] = flags & 0x71                # 0x10 swims, 0x20 flies: not dropped to the floor (0x40 walks, 0x1 biped)
    if (flags >> 10) & 3:
        info["blood"] = (flags >> 10) & 3            # 1 skeleton (white), 2 metal (gold sparks); 0 red
    if rec.tag == "CREA" and aidt and len(aidt) >= 12:
        # creatures trade and train too (Creeper, the mudcrab merchant): their AIDT services
        info["services"] = struct.unpack_from("<I", aidt, 8)[0] & 0x3FFFF
    return info


def _npc_info(rec, db):
    if rec.tag == "CREA":
        return dict({"id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "race": "", "class": "", "faction": "",
                     "rank": 0, "female": 0, "disposition": 0, "reputation": 0,
                     "script": (rec.zstr("SCRI") or "").lower(),
                     # soul size (NPDT: type, level, 8 attributes, health, magicka, fatigue, soul)
                     "soul": struct.unpack_from("<i", rec.get("NPDT"), 52)[0] if len(rec.get("NPDT") or b"") >= 56 else 0,
                     "resist_normal": normal_weapon_resistance(rec, db)},
                    **creature_stats(rec, db["SPEL"]))
    npdt = rec.get("NPDT")
    if len(npdt) == 12:           # autocalculated stats
        level, disp, rep, rank = struct.unpack_from("<hbbb", npdt)
    else:
        level = struct.unpack_from("<h", npdt)[0]
        disp, rep, rank = npdt[44], npdt[45], npdt[46]
    return {
        "id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "race": rec.zstr("RNAM") or "",
        "class": rec.zstr("CNAM") or "", "faction": rec.zstr("ANAM") or "", "rank": rank,
        "female": i32(rec.get("FLAG")) & 1, "level": level, "disposition": disp, "reputation": rep,
        "autocalc": 1 if i32(rec.get("FLAG")) & 0x10 else 0,      # spells chosen by the engine (autocalc)
        "script": (rec.zstr("SCRI") or "").lower(),
        **npc_stats(rec, db),
    }


# ---------------------------------------------------------------- magic effects

# Magic effects' GMST names (sEffect<key>) by index: their display names
EFFECT_KEYS = """WaterBreathing SwiftSwim WaterWalking Shield FireShield LightningShield FrostShield Burden Feather Jump Levitate SlowFall Lock Open FireDamage ShockDamage FrostDamage DrainAttribute DrainHealth DrainSpellpoints DrainFatigue DrainSkill DamageAttribute DamageHealth DamageMagicka DamageFatigue DamageSkill Poison WeaknesstoFire WeaknesstoFrost WeaknesstoShock WeaknesstoMagicka WeaknesstoCommonDisease WeaknesstoBlightDisease WeaknesstoCorprusDisease WeaknesstoPoison WeaknesstoNormalWeapons DisintegrateWeapon DisintegrateArmor Invisibility Chameleon Light Sanctuary NightEye Charm Paralyze Silence Blind Sound CalmHumanoid CalmCreature FrenzyHumanoid FrenzyCreature DemoralizeHumanoid DemoralizeCreature RallyHumanoid RallyCreature Dispel Soultrap Telekinesis Mark Recall DivineIntervention AlmsiviIntervention DetectAnimal DetectEnchantment DetectKey SpellAbsorption Reflect CureCommonDisease CureBlightDisease CureCorprusDisease CurePoison CureParalyzation RestoreAttribute RestoreHealth RestoreSpellPoints RestoreFatigue RestoreSkill FortifyAttribute FortifyHealth FortifySpellpoints FortifyFatigue FortifySkill FortifyMagickaMultiplier AbsorbAttribute AbsorbHealth AbsorbSpellPoints AbsorbFatigue AbsorbSkill ResistFire ResistFrost ResistShock ResistMagicka ResistCommonDisease ResistBlightDisease ResistCorprusDisease ResistPoison ResistNormalWeapons ResistParalysis RemoveCurse TurnUndead SummonScamp SummonClannfear SummonDaedroth SummonDremora SummonAncestralGhost SummonSkeletalMinion SummonLeastBonewalker SummonGreaterBonewalker SummonBonelord SummonWingedTwilight SummonHunger SummonGoldenSaint SummonFlameAtronach SummonFrostAtronach SummonStormAtronach FortifyAttackBonus CommandCreatures CommandHumanoids BoundDagger BoundLongsword BoundMace BoundBattleAxe BoundSpear BoundLongbow ExtraSpell BoundCuirass BoundHelm BoundBoots BoundShield BoundGloves Corpus Vampirism SummonCenturionSphere SunDamage StuntedMagicka""".split()


def magic_effects(db):
    """School, base cost, flags (MEDT: 0x4 no duration, 0x8 no magnitude, 0x10 harmful) and display name."""
    flags, vfx, rgb = {}, {}, {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"MGEF"}):
        k = struct.unpack("<i", r.get("INDX"))[0]
        flags[k] = struct.unpack_from("<i", r.get("MEDT"), 8)[0]
        rgb[k] = list(struct.unpack_from("<iii", r.get("MEDT"), 12))      # its colour (enchanted items glow in it)
        # the visuals: statics named by the effect (cast at the hand, the bolt, the hit, the area)
        vfx[k] = {t.lower(): (r.zstr(t) or "").lower() for t in ("CVFX", "BVFX", "HVFX", "AVFX") if r.zstr(t)}
        # its own sounds (shock, frost; some borrow another school's), else the school's
        vfx[k].update({t.lower(): r.zstr(t).lower() for t in ("CSND", "BSND", "HSND", "ASND") if r.zstr(t)})
    out = {}
    for k, v in db["MGEF"].items():
        name = db["GMST"].get("seffect" + EFFECT_KEYS[k].lower(), "") if k < len(EFFECT_KEYS) else ""
        out[str(k)] = dict(v, flags=flags.get(k, 0) | hardcoded_flags(k), name=name or f"Effect {k}", rgb=rgb.get(k, [255, 255, 255]),
                           **vfx.get(k, {}))
    return out


# Effects with OpenMW's "applied once" flag 0x1000 (set in code by its loader, not in the ESM: the HardcodedFlags table in
# components/esm3/loadmgef.cpp): the stat changes that are undone at the end, cures, summons, bound items, detects,
# resists and so on. Their duration counts as it is; others count at least 1
APPLIED_ONCE = set(range(0, 14)) | {17, 18, 19, 20, 21} | set(range(28, 37)) | set(range(39, 69)) | set(range(79, 86)) | {89}     | set(range(90, 100)) | {101} | set(range(102, 135)) | set(range(136, 143))


def hardcoded_flags(k):
    """What the engine knows about an effect that its record doesn't: 0x4 no duration, 0x8 no magnitude,
    0x10 harmful, 0x40 / 0x80 / 0x100 castable on self / touch / target."""
    no_magnitude = {0, 2, 39, 45, 46, 58, 60, 61, 62, 63, 69, 70, 71, 72, 73, 132, 133, 134} | set(range(102, 117))         | set(range(120, 132))
    no_duration = {12, 13, 57, 60, 61, 62, 63, 69, 70, 71, 72, 73}
    harmful = {7, 45, 46, 47, 48, 51, 52, 53, 54, 58, 135, 136} | set(range(14, 39)) | set(range(85, 90))
    self_only = {43, 59, 60, 61, 62, 63, 64, 65, 66, 126, 132, 133, 134} | set(range(102, 117)) | set(range(120, 132))
    others_only = {12, 13, 44, 58, 101, 118, 119} | set(range(49, 57)) | set(range(85, 90))
    f = 0
    f |= 0x1000 if k in APPLIED_ONCE else 0
    f |= 0x4 if k in no_duration else 0
    f |= 0x8 if k in no_magnitude else 0
    f |= 0x10 if k in harmful else 0
    f |= 0x40 if k not in others_only else 0
    f |= 0x180 if k not in self_only else 0
    return f


# ---------------------------------------------------------------- dialogue

def info_conditions(info):
    """[(type char, function code or name, op char, value)] from SCVR + INTV/FLTV pairs."""
    out, subs = [], info.subs
    for i, (t, d) in enumerate(subs):
        if t != "SCVR":
            continue
        s = d.decode("latin-1")
        val = 0
        if i + 1 < len(subs) and subs[i + 1][0] in ("INTV", "FLTV"):
            nd = subs[i + 1][1]
            val = i32(nd) if subs[i + 1][0] == "INTV" else f32(nd)
        out.append((s[1], s[2:4], s[4], s[5:], val))
    return out


def compare(op, a, b):
    return {"0": a == b, "1": a != b, "2": a > b, "3": a >= b, "4": a < b, "5": a <= b}.get(op, False)


def static_match(info, actor, dial_type):
    """Resolves the filters that only depend on who is speaking (and the cell they stand in).
    None = never matches."""
    onam = info.zstr("ONAM")
    if onam and onam.lower() != actor["id"].lower():
        return None
    if info.zstr("RNAM") and info.zstr("RNAM").lower() != actor["race"].lower():
        return None
    if info.zstr("CNAM") and info.zstr("CNAM").lower() != actor["class"].lower():
        return None
    fnam = info.zstr("FNAM")
    if fnam:
        if fnam.upper() == "FFFF":
            if actor["faction"]:
                return None
        elif fnam.lower() != actor["faction"].lower():
            return None
    anam = info.zstr("ANAM")
    if anam and not any(c.lower().startswith(anam.lower()) for c in actor["cells"]):
        return None
    data = info.get("DATA")
    rank, gender, pc_rank = struct.unpack_from("<bbb", data, 8)
    if gender != -1 and gender != actor["female"]:
        return None
    # the speaker's rank: in the INFO's faction, or with none named in their own (-1 without one); not asked
    # of a "no faction" line (OpenMW's testActor)
    if rank != -1 and (fnam or "").upper() != "FFFF" and (actor["rank"] if actor["faction"] else -1) < rank:
        return None
    dynamic = []
    # The player's faction (joined while playing): member of DNAM at least at pc_rank, or at least
    # pc_rank in the speaker's faction (checked at run time: 'F' / 'G' conditions)
    dnam = info.zstr("DNAM")
    if dnam and dnam.upper() != "FFFF":
        dynamic.append(["F", dnam.lower(), "3", max(pc_rank, 0)])
    elif pc_rank != -1:
        dynamic.append(["G", "", "3", pc_rank])
    for kind, func, op, name, val in info_conditions(info):
        if kind in "789A":
            nl = name.lower()
            v = {"7": actor["id"].lower() != nl, "8": actor["faction"].lower() != nl,
                 "9": actor["class"].lower() != nl, "A": actor["race"].lower() != nl}[kind]
            if not v:           # OpenMW ignores the operator and value of the Not family: only the plain truth counts
                return None
        dynamic.append([kind, func if kind == "1" else name.lower(), op, val])
    return dynamic


def speaker_filters(info):
    """Who may say a response, checked against the speaker at run time: their id, race, class, faction
    ("ffff": none), the cell they stand in (name prefix), sex, least faction rank."""
    out = {}
    for key, sub in (("id", "ONAM"), ("race", "RNAM"), ("class", "CNAM"), ("faction", "FNAM"), ("cell", "ANAM")):
        v = info.zstr(sub)
        if v:
            out[key] = v.lower()
    rank, gender = struct.unpack_from("<bb", info.get("DATA"), 8)
    if gender != -1:
        out["sex"] = gender
    if rank != -1:
        out["rank"] = rank
    return out


# Voiced lines people say on their own: greetings, idle muttering, and what they shout when they attack,
# are hit, flee, catch a thief or an intruder
VOICE_TOPICS = ("hello", "idle", "attack", "hit", "flee", "thief", "intruder")


def build_dialogue(db, actors, locals_by_actor, globals_, errors):
    """Topics, greetings and voice lines any of the level's actors can say."""
    topics = []
    for dial in db["dialogue"]:
        dtype = dial["type"]
        if dtype not in (0, 1, 2, 3):
            continue                # 3: persuasion reactions and service / info refusals
        if dtype == 1 and dial["name"].lower() not in VOICE_TOPICS:
            continue           # (Alarm has one line; unused)
        infos = []
        for info in dial["infos"]:
            who, dynamic = [], None
            for ai, actor in enumerate(actors):
                if actor.get("creature") and (info.zstr("ONAM") or "").lower() != actor["id"].lower():
                    continue            # creatures say only what names them (Vivec, Yagrum Bagarn, Creeper)
                d = static_match(info, actor, dtype)
                if d is not None:
                    who.append(ai)
                    dynamic = d
            if not who:
                continue
            script_src = info.zstr("BNAM") or ""
            names = sorted({n for ai in who for n in locals_by_actor.get(ai, [])})
            try:
                script = compile_snippet(script_src, globals_, names) if script_src.strip() else []
            except (ScriptError, IndexError) as e:
                errors.append(f"result script {dial['name']}/{info.zstr('INAM')}: {e}")
                script = []
            infos.append({
                "who": speaker_filters(info), "text": txt(info.zstr("NAME")), "conds": dynamic,
                "disp": i32(info.get("DATA"), 4), "script": script,
                "sound": info.zstr("SNAM") or "",
            })
        if infos:
            topics.append({"name": dial["name"], "type": dtype, "infos": infos})
    return topics


def plugin_journal_infos(quest_ids):
    """Journal INFOs of Tribunal.esm and Bloodmoon.esm for the given quests, as {quest: {INAM: record}}.
    Morrowind.esm carries no QSTN / QSTF / QSTR at all: the quest names and the finished / restart flags
    of its own quests are in these two files, which replace its entries by INAM and add new ones."""
    out = {}
    for name in ("Tribunal.esm", "Bloodmoon.esm"):
        path = DATA_FILES / name
        if not path.exists():
            continue
        cur = None
        for r in read_records(path, {"DIAL", "INFO"}):
            if r.tag == "DIAL":
                d = r.get("DATA")
                cur = r.id.lower() if d and d[0] == 4 and r.id.lower() in quest_ids else None
            elif cur is not None:
                out.setdefault(cur, {})[r.zstr("INAM")] = r
    return out


def build_journals(db, quest_ids):
    plugin = plugin_journal_infos(quest_ids)
    out = {}
    for dial in db["dialogue"]:
        if dial["type"] != 4 or dial["name"].lower() not in quest_ids:
            continue
        key = dial["name"].lower()
        # Entries by INAM: the base file's order, a plugin's version replacing it, its new ones last
        infos = {i.zstr("INAM"): i for i in dial["infos"]}
        infos.update(plugin.get(key, {}))
        entries, title = [], ""
        for info in infos.values():
            idx = i32(info.get("DATA"), 4)
            text = txt(info.zstr("NAME"))
            if info.get("QSTN") is not None:
                title = text
                continue
            # A flag counts by its presence, as in OpenMW (QS_Finished / QS_Restart)
            entries.append({"index": idx, "text": text, "finished": info.get("QSTF") is not None,
                            "restart": info.get("QSTR") is not None})
        out[dial["name"].lower()] = {"name": dial["name"], "title": title, "entries": entries}
    return out


# ---------------------------------------------------------------- scripts

def walk_calls(node, fn):
    if isinstance(node, list):
        if len(node) >= 4 and node[0] == "c":
            fn(node)
        for x in node:
            walk_calls(x, fn)


def string_args(tree):
    out = set()
    walk_calls(tree, lambda n: out.update(a[1] for a in n[3] if isinstance(a, list) and a and a[0] == "s"))
    return out


# ---------------------------------------------------------------- records

def body_part_flags(rec):
    """Armor / clothing parts a beast race cannot wear (OpenMW canBeEquipped): 1 a head part (a full helm), 2 a foot part."""
    idx = [d[0] for tt, d in rec.subs if tt == "INDX" and d]
    return (1 if 0 in idx else 0) | (2 if 15 in idx or 16 in idx else 0)


def object_entry(rec):
    t = rec.tag
    # A nameless activator (smoke, the chargen room and boat) keeps no name: the crosshair names nothing
    e = {"id": rec.id, "type": t, "name": txt(rec.zstr("FNAM")) or ("" if t == "ACTI" else rec.id),
         "script": (rec.zstr("SCRI") or "").lower()}
    if t in ITEM_TYPES:
        # (a potion keeps its icon in TEXT, every other item in ITEX)
        e["icon"] = rec.zstr("TEXT" if t == "ALCH" else "ITEX") or ""
    if t in ("WEAP", "ARMO", "CLOT", "BOOK") and rec.zstr("ENAM"):
        e["magic"] = 1                     # enchanted: the Magic tab, the magic icon background
        e["ench"] = rec.zstr("ENAM").lower()
    # enchantment capacity (what enchanting can put into it)
    cap = {"WEAP": ("WPDT", "<h", 20), "ARMO": ("AODT", "<i", 16), "CLOT": ("CTDT", "<h", 10),
           "BOOK": ("BKDT", "<i", 16)}.get(t)
    # (the field's own size: CTDT is 12 bytes, its capacity a short at 10; + 4 left clothing with none)
    if cap and rec.get(cap[0]) and len(rec.get(cap[0])) >= cap[2] + struct.calcsize(cap[1]):
        e["enchant"] = struct.unpack_from(cap[1], rec.get(cap[0]), cap[2])[0]
    data = {"MISC": "MCDT", "BOOK": "BKDT", "ALCH": "ALDT", "INGR": "IRDT", "APPA": "AADT",
            "LOCK": "LKDT", "PROB": "PBDT", "REPA": "RIDT"}.get(t)
    if data and rec.get(data):
        d = rec.get(data)
        # AADT: type, quality, weight, value
        e["weight"], e["value"] = (f32(d, 8), i32(d, 12)) if t == "APPA" else (f32(d), i32(d, 4))
    if t == "REPA":
        e["uses"], e["quality"] = i32(rec.get("RIDT"), 8), f32(rec.get("RIDT"), 12)
    if t in ("LOCK", "PROB") and rec.get(data):
        e["quality"], e["uses"] = f32(rec.get(data), 8), i32(rec.get(data), 12)
    if t == "APPA" and rec.get("AADT"):
        # alchemy apparatus: 0 mortar and pestle, 1 alembic, 2 calcinator, 3 retort
        e["subtype"], e["quality"] = i32(rec.get("AADT")), f32(rec.get("AADT"), 4)
    if t == "ALCH":
        e["effects"] = enam_effects(rec)
    if t == "INGR":
        d = rec.get("IRDT")
        eff, sk, at = struct.unpack_from("<4i", d, 8), struct.unpack_from("<4i", d, 24), struct.unpack_from("<4i", d, 40)
        # alchemy: all four effects (effect, skill, attribute)
        e["ingr"] = [[eff[k], sk[k], at[k]] for k in range(4) if eff[k] >= 0]
        if eff[0] >= 0:
            e["effects"] = [{"effect": eff[0], "skill": sk[0], "attribute": at[0], "min": INGREDIENT_MAGNITUDE,
                             "max": INGREDIENT_MAGNITUDE, "duration": INGREDIENT_DURATION, "range": 0}]
    if t == "BOOK":
        e["text"] = book_text(rec.zstr("TEXT"))
        e["scroll"] = i32(rec.get("BKDT"), 8)
        if i32(rec.get("BKDT"), 12) >= 0:
            e["skill"] = i32(rec.get("BKDT"), 12)          # skill book: raises it on first read
    if t == "WEAP":
        d = rec.get("WPDT")
        e["weight"], e["value"], e["wtype"] = f32(d), i32(d, 4), struct.unpack_from("<h", d, 8)[0]
        e["health"] = struct.unpack_from("<H", d, 10)[0]
        e["speed"], e["reach"] = f32(d, 12), f32(d, 16)
        e["flags"] = i32(d, 28) if len(d) >= 32 else 0      # 1: ignores normal weapon resistance (silver)
        e.update(weapon_damage(rec))
    if t == "ARMO":
        d = rec.get("AODT")
        e["atype"], e["weight"], e["value"], e["armor"] = i32(d), f32(d, 4), i32(d, 8), i32(d, 20)
        e["health"] = i32(d, 12)
        e["flags"] = body_part_flags(rec)
    if t == "CLOT":
        d = rec.get("CTDT")
        e["ctype"], e["weight"], e["value"] = i32(d), f32(d, 4), struct.unpack_from("<h", d, 8)[0]
        e["flags"] = body_part_flags(rec)
    if t == "LIGH":
        d = rec.get("LHDT")
        e["weight"], e["value"], e["flags"] = f32(d), i32(d, 4), i32(d, 20)
        e["sound"] = rec.zstr("SNAM") or ""
        e["radius"], e["color"] = i32(d, 12), list(d[16:19])     # what it lights when carried
    if t == "DOOR":
        e["open_sound"], e["close_sound"] = rec.zstr("SNAM") or "", rec.zstr("ANAM") or ""
    if t == "CONT":
        e["items"] = [[i32(d), z(d[4:])] for tt, d in rec.subs if tt == "NPCO"]
        e["organic"] = i32(rec.get("FLAG")) & 1 if rec.get("FLAG") else 0
        e["respawn"] = 1 if rec.get("FLAG") and i32(rec.get("FLAG")) & 2 else 0   # restocks (flora, merchants' chests)
        e["capacity"] = round(f32(rec.get("CNDT")), 3) if rec.get("CNDT") else 0   # weight it holds (what the player can put in)
    return e


def class_entry(rec):
    d = struct.unpack("<15i", rec.get("CLDT"))
    return {"id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "desc": txt(rec.zstr("DESC")),
            "attributes": list(d[0:2]), "specialization": d[2],
            "minor": [d[3 + 2 * i] for i in range(5)], "major": [d[4 + 2 * i] for i in range(5)],
            "playable": d[13] & 1}


def race_entry(rec):
    d = rec.get("RADT")
    bonuses = [list(struct.unpack_from("<ii", d, 8 * i)) for i in range(7)]
    attrs = [list(struct.unpack_from("<ii", d, 56 + 8 * i)) for i in range(8)]
    height, weight = struct.unpack_from("<2f", d, 120), struct.unpack_from("<2f", d, 128)
    flags = i32(d, 136)
    return {"id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "desc": txt(rec.zstr("DESC")),
            "skill_bonus": [b for b in bonuses if b[0] >= 0],
            "attributes": attrs, "height": list(height), "weight": list(weight),
            "playable": flags & 1, "beast": (flags >> 1) & 1,
            "spells": [z(dd) for tt, dd in rec.subs if tt == "NPCS"]}


def enam_effects(rec):
    """ENAM effect list of a spell / potion / enchantment. range: 0 self, 1 touch, 2 target."""
    effects = []
    for tt, dd in rec.subs:
        if tt == "ENAM":
            eid, skill, attr, rng, area, dur, mn, mx = struct.unpack_from("<hbbiiiii", dd)
            effects.append({"effect": eid, "skill": skill, "attribute": attr, "min": mn, "max": mx,
                            "duration": dur, "range": rng, **({"area": area} if area else {})})
    return effects


def enchantments(objects):
    """ENCH records the exported items carry: type (0 cast once, 1 on strike, 2 on use, 3 constant),
    cost per use, charge, effects."""
    wanted = {o["ench"] for o in objects.values() if o.get("ench")}
    out = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"ENCH"}):
        if r.id.lower() in wanted:
            d = r.get("ENDT")
            out[r.id.lower()] = {"type": i32(d), "cost": i32(d, 4), "charge": i32(d, 8), "effects": enam_effects(r)}
    return out


def faction_reactions(rec):
    """How a faction feels about others: ANAM (the other faction) then INTV (reaction) pairs."""
    out, other = {}, None
    for t, d in rec.subs:
        if t == "ANAM":
            other = z(d).lower()
        elif t == "INTV" and other is not None:
            out[other] = i32(d)
            other = None
    return out


def spell_entry(rec):
    d = rec.get("SPDT")
    return {"id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "type": i32(d), "cost": i32(d, 4),
            "flags": i32(d, 8), "effects": enam_effects(rec)}


INGREDIENT_MAGNITUDE, INGREDIENT_DURATION = 5, 10     # eating raw: a weak dose of the first effect


def build(db, cells, actor_records, actor_cells):
    """cells: [{"name", "file", "interior", "refs": [{"id", "type", ...}], "npcs": [NPC_ records]}], the
    level's cells, the starting one first. actor_records: one NPC_ / CREA record per actor entry (the
    references' "actor" indices), actor_cells: the cells each is placed in."""
    errors = []
    # Short / long globals: the engine truncates the stored float (some hold leftover bytes, e.g.
    # DestroyBlight 2e-35, which must read as 0 or the ending never starts)
    globals_ = {k: (t, (float(int(v)) if math.isfinite(v) else 0.0) if t in ("s", "l") else v)
                for k, (t, v) in db["GLOB"].items()}
    glob_names = {k: v[0] for k, v in globals_.items()}      # name -> type, for the compiler's whole-number division

    # Actors and their scripts' locals
    actors = [dict(npc_info(r, db), cells=sorted(actor_cells[i])) for i, r in enumerate(actor_records)]
    scripts_src = db["SCPT"]
    compiled = {}

    def compile_named(name):
        name = name.lower()
        if name in compiled or name not in scripts_src:
            return
        try:
            compiled[name] = compile_script(scripts_src[name], glob_names)
        except (ScriptError, IndexError) as e:
            errors.append(f"script {name}: {e}")
            return
        # follow StartScript / StopScript to global scripts
        def follow(n):
            if n[2] in ("startscript",) and n[3] and isinstance(n[3][0], list):
                compile_named(n[3][0][1])
        walk_calls(compiled[name]["body"], follow)

    object_ids = {r["id"].lower() for c in cells for r in c["refs"]}
    for oid in list(object_ids):
        rec = db["objects"].get(oid)
        if rec is not None and rec.zstr("SCRI"):
            compile_named(rec.zstr("SCRI"))
    # Every other script too: dialogue and quests start global scripts (StartScript) by name
    for name in list(scripts_src):
        compile_named(name)
    locals_by_actor = {i: [n for _, n in compiled[a["script"]]["locals"]] for i, a in enumerate(actors)
                       if a["script"] in compiled}

    dialogue = build_dialogue(db, actors, locals_by_actor, glob_names, errors)

    # Everything scripts and dialogue can name: items, quests, sounds, scripts
    trees = [s["body"] for s in compiled.values()] + [i["script"] for t in dialogue for i in t["infos"]]
    names = set()
    for tree in trees:
        names |= {n.lower() for n in string_args(tree)}
    for t in dialogue:
        for i in t["infos"]:
            for kind, name, _, _ in i["conds"]:
                if kind in "456":
                    names.add(str(name).lower())
    # Scripts of items the level can hand out
    for n in list(names):
        rec = db["objects"].get(n)
        if rec is not None and rec.zstr("SCRI"):
            compile_named(rec.zstr("SCRI"))
    for tree in [s["body"] for s in compiled.values()]:
        names |= {n.lower() for n in string_args(tree)}

    objects = {}
    for oid in object_ids | names:
        rec = db["objects"].get(oid)
        if rec is not None:
            objects[oid] = object_entry(rec)
            if rec.tag == "CONT":
                for _, item in objects[oid]["items"]:
                    ir = db["objects"].get(item.lower())
                    if ir is not None:
                        objects[item.lower()] = object_entry(ir)
    bind(db)        # the leveled lists in an NPC's inventory become the items they wear and carry
    for a in actors:
        rec = db["objects"][a["id"].lower()]
        items = npc_items(rec)
        objects[a["id"].lower()] = {"id": a["id"], "type": rec.tag, "name": a["name"], "script": a["script"],
                                    "items": [[n, i] for n, i in items]}
        # Their inventory can be bought or looted
        for _, item in items:
            ir = db["objects"].get(item.lower())
            if ir is not None and ir.tag in ITEM_TYPES:
                objects[item.lower()] = object_entry(ir)

    # Leveled item entries in containers and inventories stay lists (picked by the player's level at
    # run time: "leveled_items"); every item they can give is exported
    leveled_items = {}
    levi = {r.id.lower(): r for r in read_records(DATA_FILES / "Morrowind.esm", {"LEVI"})}

    def add_list(lid, depth=0):
        if lid in leveled_items or lid not in levi or depth > 5:
            return
        r = levi[lid]
        flags = i32(r.get("DATA")) if r.get("DATA") else 0
        entries = [[lvl, iid.lower()] for lvl, iid in db["LEVI"].get(lid, [])]
        leveled_items[lid] = {"all": flags & 1, "each": 1 if flags & 2 else 0,
                              "none": (r.get("NNAM") or bytes(1))[0], "e": entries}
        for _, iid in entries:
            if iid in levi:
                add_list(iid, depth + 1)
            else:
                ir = db["objects"].get(iid)
                if ir is not None and ir.tag in ITEM_TYPES and iid not in objects:
                    objects[iid] = object_entry(ir)

    for o in list(objects.values()):
        if not o.get("items"):
            continue
        kept = []
        for count, item in o["items"]:
            rec = db["objects"].get(item.lower())
            if rec is None:
                if item.lower() in levi:
                    add_list(item.lower())
                    kept.append([count, item.lower()])
                continue
            if rec.tag in ITEM_TYPES and item.lower() not in objects:
                objects[item.lower()] = object_entry(rec)
            kept.append([count, item])
        o["items"] = kept

    quest_ids = {n for n in names if any(d["type"] == 4 and d["name"].lower() == n for d in db["dialogue"])}
    journals = build_journals(db, quest_ids)

    # Sounds: sound records by id, plus raw files (voice lines)
    # Starting kit: what the "player" record carries (clothes), plus a weapon and some gold,
    # which a game started past character creation hands out
    player_rec = db["objects"].get("player")
    player_items = [[abs(i32(d)), z(d[4:])] for t, d in (player_rec.subs if player_rec else []) if t == "NPCO"]
    starter_weapon = next((w for w in STARTER_WEAPONS if w in db["objects"]), None)
    extra_items = ([[1, starter_weapon]] if starter_weapon else []) + [[STARTER_GOLD, "gold_001"]]
    for _, item in player_items + extra_items:
        rec = db["objects"].get(item.lower())
        if rec is not None:
            objects[item.lower()] = object_entry(rec)

    # Bound weapons and armor (summoned by spells)
    for bid in BOUND_ITEMS:
        rec = db["objects"].get(bid)
        if rec is not None:
            objects[bid] = object_entry(rec)

    sound_ids = {s.lower() for s in ENGINE_SOUNDS}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"MGEF"}):
        sound_ids.update(r.zstr(t).lower() for t in ("CSND", "BSND", "HSND", "ASND") if r.zstr(t))
    for school in ("alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration"):
        sound_ids.update(f"{school} {kind}" for kind in ("cast", "bolt", "hit", "area"))
    water_sound = pick_water_sound(db)
    if water_sound:
        sound_ids.add(water_sound)
    for o in objects.values():
        for k in ("sound", "open_sound", "close_sound"):
            if o.get(k):
                sound_ids.add(o[k].lower())
    for tree in trees + [s["body"] for s in compiled.values()]:
        walk_calls(tree, lambda n: sound_ids.update(
            a[1].lower() for a in n[3][:1] if n[2] in ("playsound", "playsound3d", "playsoundvp", "playsound3dvp",
                                                       "playloopsound3d", "playloopsound3dvp") and isinstance(a, list)))
    sounds = {}
    for sid in sound_ids:
        rec = db["SOUN"].get(sid)
        if rec is not None:
            d = rec.get("DATA")
            sounds[sid] = {"file": rec.zstr("FNAM"), "volume": d[0] / 255.0, "min": d[1], "max": d[2]}
    voice_files = set()
    walk_calls(trees + [s["body"] for s in compiled.values()],
               lambda n: voice_files.add(n[3][0][1]) if n[2] == "say" and n[3] and isinstance(n[3][0], list) else None)
    for t in dialogue:
        for i in t["infos"]:
            if i["sound"]:
                voice_files.add(i["sound"])

    skills = []
    for idx in range(27):
        rec = db["SKIL"].get(idx)
        d = rec.get("SKDT")
        skills.append({"attribute": i32(d), "specialization": i32(d, 4), "desc": txt(rec.zstr("DESC")),
                       "use": [round(f32(d, 8 + 4 * k), 3) for k in range(4)]})

    spells_needed = set()
    birthsigns = []
    for rec in db["BSGN"].values():
        sp = [z(d) for t, d in rec.subs if t == "NPCS"]
        spells_needed.update(s.lower() for s in sp)
        birthsigns.append({"id": rec.id, "name": txt(rec.zstr("FNAM")) or rec.id, "desc": txt(rec.zstr("DESC")),
                           "texture": rec.zstr("TNAM") or "", "spells": sp})
    races = [race_entry(r) for r in db["RACE"].values()]
    for r in races:
        spells_needed.update(s.lower() for s in r["spells"])
    # Spells the level's spell merchants sell (their NPCS list)
    for a in actors:
        if a.get("services", 0) & 0x800:
            rec = db["objects"].get(a["id"].lower())
            a["spells"] = [z(d) for t, d in rec.subs if t == "NPCS"] if rec is not None else []
            spells_needed.update(s.lower() for s in a["spells"])
    # Spells the level's actors cast in combat; diseases they carry (caught from their blows)
    for a in actors:
        spells_needed.update(s.lower() for s in a.get("combat_spells", []))
        rec = db["objects"].get(a["id"].lower())
        dis = [z(d).lower() for t, d in (rec.subs if rec is not None else []) if t == "NPCS"]
        a["diseases"] = [s for s in dis if s in db["SPEL"] and i32(db["SPEL"][s].get("SPDT")) in (2, 3)]
        spells_needed.update(a["diseases"])
    # Spells scripts and dialogue name (AddSpell: diseases, abilities, gifts)
    spells_needed.update(n for n in names if n in db["SPEL"])
    # What autocalc may pick (spells flagged Autocalc for NPCs, PC Start for a new player)
    spells_needed.update(sid for sid, sp in db["SPEL"].items()
                         if sp.get("SPDT") and i32(sp.get("SPDT")) == 0 and i32(sp.get("SPDT"), 8) & 3)
    # Traps on doors and containers (TNAM): cast on whoever opens them
    for cell in db["CELL"].values():
        for tag, d in cell.subs:
            if tag == "TNAM":
                spells_needed.add(d.split(bytes(1), 1)[0].decode("latin-1").lower())
    spells = {s: spell_entry(db["SPEL"][s]) for s in spells_needed if s in db["SPEL"]}

    # Every faction: the player can join them (PCJoinFaction), dialogue names their ranks
    factions = {}
    for fid, rec in db["FACT"].items():
        fadt = rec.get("FADT")
        # FADT: favoured attributes, per rank (attribute 1, attribute 2, primary skill, favoured skill,
        # faction reputation) and the favoured skills: what promotion asks for
        factions[fid] = {"name": txt(rec.zstr("FNAM")) or rec.id,
                         "ranks": [txt(z(d)) for t, d in rec.subs if t == "RNAM"],
                         "attrs": list(struct.unpack_from("<2i", fadt, 0)) if fadt else [],
                         "reqs": [list(struct.unpack_from("<5i", fadt, 8 + 20 * k)) for k in range(10)] if fadt else [],
                         "skills": [x for x in struct.unpack_from("<7i", fadt, 208) if x >= 0] if fadt else [],
                         "reactions": faction_reactions(rec)}

    for a in actors:
        del a["cells"]
    # Object ids scripts and dialogue name -> the cells (indices into cells) that hold them, so the
    # 3DS can read those cells' objects on demand instead of every cell at start
    named = set()

    def all_strings(node):
        if isinstance(node, str):
            named.add(node.lower())
        elif isinstance(node, list):
            for x in node:
                all_strings(x)
        elif isinstance(node, dict):
            for x in node.values():
                all_strings(x)
    all_strings([s["body"] for s in compiled.values()] + [i["script"] for t in dialogue for i in t["infos"]])
    named |= names
    ref_cells = {}
    for ci, c in enumerate(cells):
        for r in c["refs"]:
            idl = r["id"].lower()
            if idl in named and ci not in ref_cells.get(idl, []):
                ref_cells.setdefault(idl, []).append(ci)

    game = {
        "ref_cells": ref_cells,
        "cell": cells[0]["name"],
        "cells": [dict({"name": c["name"], "file": c["file"], "interior": c["interior"]},
                       **({"grid": c["grid"], "sea": c.get("sea", 0.0)} if "grid" in c else {}),
                       **({"nosleep": True} if c.get("nosleep") else {})) for c in cells],
        "factions": factions,
        "actors": actors,
        "objects": objects,
        "scripts": compiled,
        "dialogue": dialogue,
        "journal": journals,
        "globals": {k: {"type": v[0], "value": v[1]} for k, v in globals_.items()},
        "gmst": {k: txt(v) if isinstance(v, str) else v for k, v in db["GMST"].items()},
        "classes": [class_entry(r) for r in db["CLAS"].values()],
        "races": races,
        "birthsigns": birthsigns,
        "spells": spells,
        "enchantments": enchantments(objects),
        "leveled_items": leveled_items,
        "skills": skills,
        "sounds": sounds,
        "magic_effects": magic_effects(db),
        "start_items": player_items,
        "skip_chargen_items": extra_items,
        "ambient": {"water": water_sound or ""},
    }
    return game, sorted(voice_files), errors
