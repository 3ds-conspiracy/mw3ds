"""NPC numbers the 3DS needs for trading and combat: attributes, skills, health, gold,
merchant services, carried items, the weapon they fight with and their armor rating.

NPCs with full stats (52-byte NPDT) store them; autocalculated ones (12-byte NPDT) are
estimated from race and class the way character creation works, plus growth per level:
major skills +2, minor +1, other skills +0.5 and attributes +2 per level above 1.
"""
import struct
import zlib

# Skill indices (SKIL INDX)
SKILL_BLUNT, SKILL_LONG_BLADE, SKILL_AXE, SKILL_SPEAR = 4, 5, 6, 7
SKILL_SHORT_BLADE, SKILL_MERCANTILE, SKILL_HAND_TO_HAND = 22, 24, 26
ATTR_STRENGTH, ATTR_WILLPOWER, ATTR_AGILITY, ATTR_ENDURANCE = 0, 2, 3, 5

# WEAP types: 0..8 melee, 9 bow, 10 crossbow, 11 thrown, 12 arrow, 13 bolt
MELEE_TYPES = range(0, 9)
BOW, CROSSBOW, THROWN, ARROW, BOLT = 9, 10, 11, 12, 13
SKILL_MARKSMAN = 23
ARMOR_SHIELD = 8
# Magic effects an actor's combat AI knows how to use: harm (fire, frost, shock, poison, damage
# health / magicka / fatigue, drain health) cast at the player, restore health on itself
HARM_EFFECTS = {14, 15, 16, 23, 24, 25, 27, 86}
HEAL_EFFECTS = {75}
# Armor rating weight per ARMO slot (helm, cuirass, pauldrons, greaves, boots, gauntlets, shield, bracers)
ARMOR_SLOT_WEIGHT = [0.1, 0.3, 0.1, 0.1, 0.1, 0.1, 0.05, 0.05, 0.1, 0.05, 0.05]


# The leveled item lists (LEVI) of the data being converted: an NPC's inventory can hold one ("Imperial Guard
# Random Weapon"), and it has to become a real item for what the NPC wears and fights with. Set by bind().
_LEVI = {}


def bind(db):
    global _LEVI
    _LEVI = db.get("LEVI", {})


def resolve_item(lid, level, seed, depth=0):
    """A leveled item list's pick for an actor of this level: among the entries at or under the level the
    highest-level ones (the list's 'all levels' flag not read), one chosen by the seed, so the same NPC
    always gets the same piece; nested lists followed. None for an empty list."""
    entries = _LEVI.get(lid.lower())
    if not entries or depth > 5:
        return None
    ok = [e for e in entries if e[0] <= level] or [min(entries)]
    top = max(e[0] for e in ok)
    best = sorted(e[1] for e in ok if e[0] == top)
    pick = best[zlib.crc32((seed + "|" + lid).lower().encode("latin-1")) % len(best)]
    if pick.lower() in _LEVI:
        return resolve_item(pick, level, seed, depth + 1)
    return pick


def npc_items(rec):
    """[(count, id)] from NPCO (negative counts are restocking merchant stock), the leveled lists among them
    resolved to an item."""
    out = []
    npdt = rec.get("NPDT")
    level = struct.unpack_from("<h", npdt)[0] if npdt else 1
    for t, d in rec.subs:
        if t == "NPCO":
            item = d[4:].split(b"\0", 1)[0].decode("latin-1")
            if item.lower() in _LEVI:
                item = resolve_item(item, level, rec.id)
                if item is None:
                    continue
            out.append((struct.unpack_from("<i", d)[0], item))      # (the sign stays: negative is restocking stock)
    return out


def weapon_damage(rec):
    d = rec.get("WPDT")
    return {"chop": [d[22], d[23]], "slash": [d[24], d[25]], "thrust": [d[26], d[27]]}


def best_weapon(rec, objects):
    """Id of the carried melee weapon with the highest maximum damage, or None (hand-to-hand)."""
    best, best_dmg = None, -1
    for _, item in npc_items(rec):
        w = objects.get(item.lower())
        if w is None or w.tag != "WEAP":
            continue
        d = w.get("WPDT")
        if struct.unpack_from("<h", d, 8)[0] not in MELEE_TYPES:
            continue
        dmg = max(d[23], d[25], d[27])
        if dmg > best_dmg:
            best, best_dmg = w.id, dmg
    return best


def weapon_type(w):
    return struct.unpack_from("<h", w.get("WPDT"), 8)[0]


def ranged_weapon(rec, objects):
    """(weapon id, ammunition id or None) of the ranged weapon an NPC can fight with, or (None, None):
    a bow / crossbow with matching ammunition, or thrown weapons (their own ammunition)."""
    items = [objects.get(i.lower()) for _, i in npc_items(rec)]
    weapons = [w for w in items if w is not None and w.tag == "WEAP"]
    for w in sorted(weapons, key=lambda w: -max(w.get("WPDT")[22:28])):
        t = weapon_type(w)
        if t == THROWN:
            return w.id, None
        if t in (BOW, CROSSBOW):
            ammo = next((a for a in weapons if weapon_type(a) == (ARROW if t == BOW else BOLT)), None)
            if ammo is not None:
                return w.id, ammo.id
    return None, None


def best_shield(rec, objects):
    best, rating = None, -1
    for _, item in npc_items(rec):
        a = objects.get(item.lower())
        if a is not None and a.tag == "ARMO" and struct.unpack_from("<i", a.get("AODT"))[0] == ARMOR_SHIELD:
            r = struct.unpack_from("<i", a.get("AODT"), 20)[0]
            if r > rating:
                best, rating = a.id, r
    return best


# Weapon type -> skill
MELEE_SKILL = {0: SKILL_SHORT_BLADE, 1: SKILL_LONG_BLADE, 2: SKILL_LONG_BLADE, 3: SKILL_BLUNT, 4: SKILL_BLUNT,
               5: SKILL_BLUNT, 6: SKILL_SPEAR, 7: SKILL_AXE, 8: SKILL_AXE}


def combat_weapon(rec, objects, skills):
    """(weapon, ammunition) an NPC fights with: their ranged weapon when their Marksman skill is at
    least that of their best melee weapon, else the best melee weapon (None = hand-to-hand)."""
    melee = best_weapon(rec, objects)
    ranged, ammo = ranged_weapon(rec, objects)
    if ranged is not None:
        melee_skill = skills[MELEE_SKILL[weapon_type(objects[melee.lower()])]] if melee else skills[SKILL_HAND_TO_HAND]
        if skills[SKILL_MARKSMAN] >= melee_skill:
            return ranged, ammo
    return melee, None


def combat_spells(rec, spel):
    """Spells from the actor's NPCS list that its combat AI casts: harmful ones at a target or by
    touch, and self-healing."""
    out = []
    for t, d in rec.subs:
        if t != "NPCS":
            continue
        sid = d.split(b"\0", 1)[0].decode("latin-1")
        sp = spel.get(sid.lower())
        if sp is None or struct.unpack_from("<i", sp.get("SPDT"))[0] not in (0, 5):     # spells and powers
            continue
        effects = [struct.unpack_from("<hbbiiiii", e) for tt, e in sp.subs if tt == "ENAM"]
        harm = any(e[0] in HARM_EFFECTS and e[3] in (1, 2) for e in effects)
        heal = any(e[0] in HEAL_EFFECTS and e[3] == 0 for e in effects)
        if harm or heal:
            out.append(sid)
    return out


# Base weights per armor slot (iHelmWeight, iCuirassWeight, iPauldronWeight x2, iGreavesWeight, iBootsWeight,
# iGauntletWeight x2, iShieldWeight, bracers as gauntlets): a piece up to 60% of it is light armor, 90% medium
ARMOR_BASE_WEIGHT = [5, 30, 10, 10, 15, 20, 5, 5, 15, 5, 5]
ARMOR_SLOT = [0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7]                 # ARMO type -> slot (a bracer takes its hand's)
SLOT_WEIGHT = [0.1, 0.3, 0.1, 0.1, 0.1, 0.1, 0.05, 0.05, 0.1]
SKILL_MEDIUM, SKILL_HEAVY, SKILL_UNARMORED, SKILL_LIGHT = 2, 3, 17, 21


def armor_rating(rec, objects, skills):
    """The armor rating as the game works it out (see spec/combat.md): each of nine slots counts the piece worn
    there (its armor x the NPC's skill in its weight class / 30), or the Unarmored rating
    (0.1 x skill) x (0.065 x skill) when the slot holds no armor."""
    unarmored = (0.1 * skills[SKILL_UNARMORED]) * (0.065 * skills[SKILL_UNARMORED])
    slots = [unarmored] * 9
    best = {}
    for _, item in npc_items(rec):
        a = objects.get(item.lower())
        if a is None or a.tag != "ARMO":
            continue
        d = a.get("AODT")
        kind, weight, rating = struct.unpack_from("<i", d)[0], struct.unpack_from("<f", d, 4)[0], struct.unpack_from("<i", d, 20)[0]
        if not 0 <= kind < len(ARMOR_SLOT):
            continue
        base = ARMOR_BASE_WEIGHT[kind]
        skill = SKILL_LIGHT if weight <= base * 0.6 + 0.0005 else SKILL_MEDIUM if weight <= base * 0.9 + 0.0005 else SKILL_HEAVY
        value = rating if weight == 0 else rating * skills[skill] / 30.0
        slot = ARMOR_SLOT[kind]
        best[slot] = max(best.get(slot, 0.0), value)
    for slot, value in best.items():
        slots[slot] = value
    return round(sum(SLOT_WEIGHT[k] * slots[k] for k in range(9)), 1)


def autocalc(level, race, cls, female):
    """(attributes[8], skills[27]) estimated from race and class."""
    attrs, skills = [40] * 8, [5] * 27
    majors, minors, favored, spec = set(), set(), set(), -1
    if cls is not None:
        d = struct.unpack("<15i", cls.get("CLDT"))
        favored, spec = set(d[0:2]), d[2]
        minors = {d[3 + 2 * i] for i in range(5)}
        majors = {d[4 + 2 * i] for i in range(5)}
    if race is not None:
        rd = race.get("RADT")
        for i in range(8):
            attrs[i] = struct.unpack_from("<ii", rd, 56 + 8 * i)[1 if female else 0]
        for i in range(7):
            skill, bonus = struct.unpack_from("<ii", rd, 8 * i)
            if 0 <= skill < 27:
                skills[skill] += bonus
    growth = max(level - 1, 0)
    for i in range(8):
        attrs[i] = min(100, attrs[i] + (10 if i in favored else 0) + 2 * growth)
    for s in range(27):
        if s in majors:
            skills[s] += 25 + 2 * growth
        elif s in minors:
            skills[s] += 10 + growth
        else:
            skills[s] += growth // 2
        if spec >= 0 and s in SPEC_SKILLS.get(spec, ()):
            skills[s] += 5
        skills[s] = min(100, skills[s])
    return attrs, skills


# Skills of each specialization (combat, magic, stealth)
SPEC_SKILLS = {0: (0, 1, 2, 3, 4, 5, 6, 7, 8), 1: (9, 10, 11, 12, 13, 14, 15, 16, 17),
               2: (18, 19, 20, 21, 22, 23, 24, 25, 26)}


def travel_destinations(rec):
    """Caravaners, boatmen, guild guides: [(pos[3], rot[3], interior cell name or None)] from DODT / DNAM."""
    out = []
    for t, d in rec.subs:
        if t == "DODT":
            v = struct.unpack("<6f", d)
            out.append([list(v[:3]), list(v[3:]), None])
        elif t == "DNAM" and out:
            out[-1][2] = d.split(b"\0", 1)[0].decode("latin-1")
    return out


def npc_stats(rec, db):
    bind(db)
    npdt = rec.get("NPDT")
    female = struct.unpack("<i", rec.get("FLAG"))[0] & 1
    if len(npdt) == 12:
        level = struct.unpack_from("<h", npdt)[0]
        gold = struct.unpack_from("<i", npdt, 8)[0]
        attrs, skills = autocalc(level, db["RACE"].get((rec.zstr("RNAM") or "").lower()),
                                 db["CLAS"].get((rec.zstr("CNAM") or "").lower()), female)
        health = int(0.5 * (attrs[ATTR_STRENGTH] + attrs[ATTR_ENDURANCE]) + 0.1 * attrs[ATTR_ENDURANCE] * max(level - 1, 0))
        fatigue = attrs[ATTR_STRENGTH] + attrs[ATTR_WILLPOWER] + attrs[ATTR_AGILITY] + attrs[ATTR_ENDURANCE]
    else:
        attrs, skills = list(npdt[2:10]), list(npdt[10:37])
        health, _, fatigue = struct.unpack_from("<3H", npdt, 38)
        gold = struct.unpack_from("<i", npdt, 48)[0]
    aidt = rec.get("AIDT")
    fight = aidt[2] if aidt else 30
    flee = aidt[3] if aidt else 30
    services = struct.unpack_from("<I", aidt, 8)[0] if aidt and len(aidt) >= 12 else 0
    weapon, ammo = combat_weapon(rec, db["objects"], skills)
    magicka = int(attrs[1] * 2.0)                       # intelligence x fNPCbaseMagickaMult
    return {"attributes": attrs, "skills": skills, "health": max(int(health), 0), "fatigue": max(int(fatigue), 1),
            "magicka": magicka, "gold": max(gold, 0), "fight": fight, "flee": flee, "services": services,
            "weapon": weapon or "", "ammo": ammo or "", "shield": best_shield(rec, db["objects"]) or "",
            "combat_spells": combat_spells(rec, db["SPEL"]), "armor": armor_rating(rec, db["objects"], skills),
            "travel": travel_destinations(rec),
            # AI_W: how far from their spot they stroll (0 = they stay put)
            "wander": struct.unpack_from("<H", rec.get("AI_W"))[0] if rec.get("AI_W") else 0,
            **({"ai": ai_package(rec)} if ai_package(rec) else {})}


def ai_package(rec):
    """The record's first AI package when it isn't wandering (that's "wander"): travel, follow, escort or
    activate, as [type (1 travel, 2 escort, 3 follow, 4 activate), target id, x, y, z, duration hours]."""
    for tag, d in rec.subs:
        if tag == "AI_W":
            return None
        if tag == "AI_T":
            x, y, z = struct.unpack_from("<3f", d)
            return [1, "", x, y, z, 0]
        if tag in ("AI_F", "AI_E"):
            x, y, z = struct.unpack_from("<3f", d)
            dur = struct.unpack_from("<H", d, 12)[0]
            target = d[14:46].split(b"\0", 1)[0].decode("latin-1").lower()
            return [3 if tag == "AI_F" else 2, target, x, y, z, dur]
        if tag == "AI_A":
            return [4, d[:32].split(b"\0", 1)[0].decode("latin-1").lower(), 0, 0, 0, 0]
    return None
