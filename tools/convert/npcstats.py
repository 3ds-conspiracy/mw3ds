"""NPC numbers the 3DS needs for trading and combat: attributes, skills, health, gold,
merchant services, carried items, the weapon they fight with and their armor rating.

NPCs with full stats (52-byte NPDT) store them; autocalculated ones (12-byte NPDT) get them from
race, class and level by OpenMW's rule (autocalc below).
"""
import struct
import zlib

from mwfiles import text, text_bytes

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
# The harmful effects (OpenMW's Harmful flag, its HardcodedFlags table in components/esm3/loadmgef.cpp: Burden, the
# damages, drains, weaknesses, disintegrates, Paralyze, Silence, Blind, Sound, Soul Trap, Turn Undead, the absorbs; not
# Calm, Frenzy, Demoralize or Rally, nor Sun Damage or Stunted Magicka): a fighter casts a spell with one at a target or
# by touch, the engine weighing which (spellpriority.cpp). build_game.py flags them harmful (0x10)
HARMFUL_EFFECTS = {7, 45, 46, 47, 48, 58, 101} | set(range(14, 39)) | set(range(85, 90))
HEAL_EFFECTS = {75}
# Armor rating weight per ARMO slot (helm, cuirass, pauldrons, greaves, boots, gauntlets, shield, bracers)
ARMOR_SLOT_WEIGHT = [0.1, 0.3, 0.1, 0.1, 0.1, 0.1, 0.05, 0.05, 0.1, 0.05, 0.05]


# The leveled item lists (LEVI) of the data being converted: an NPC's inventory can hold one ("Imperial Guard
# Random Weapon"), and it has to become a real item for what the NPC wears and fights with. Set by bind().
_LEVI = {}
# The player level the lists are picked for. OpenMW picks an actor's lists for the player's level when their cell first
# loads (MWMechanics::getLevelledItem), and the NPC wears the best of what came: the look is baked here, so it is a new
# character's pick, as a new game meets them (Baladas Demnevanni's level 10-14 Telvanni list gave him the enchanted
# Devil Cephalopod Helm when picked for his own level, 35; OpenMW gives him nothing from it before player level 10)
PICK_LEVEL = 1


def bind(db):
    global _LEVI, _SKILLS, _MAGICKA_MULT
    _LEVI = db.get("LEVI", {})
    # Each skill's governing attribute and specialization (SKIL SKDT), for autocalc
    _SKILLS = {i: struct.unpack_from("<ii", r.get("SKDT")) for i, r in db.get("SKIL", {}).items()}
    _MAGICKA_MULT = float(db.get("GMST", {}).get("fnpcbasemagickamult", 2.0))


_SKILLS = {}
_MAGICKA_MULT = 2.0


def resolve_item(lid, level, seed, depth=0):
    """A leveled item list's pick for this player level: among the entries at or under the level the
    highest-level ones (the list's 'all levels' flag not read), one chosen by the seed, so the same NPC
    always gets the same piece; nested lists followed. None for an empty list, or one whose entries are all above
    the level (OpenMW's getLevelledItem gives nothing then)."""
    entries = _LEVI.get(lid.lower())
    if not entries or depth > 5:
        return None
    ok = [e for e in entries if e[0] <= level]
    if not ok:
        return None
    top = max(e[0] for e in ok)
    best = sorted(e[1] for e in ok if e[0] == top)
    pick = best[zlib.crc32(text_bytes((seed + "|" + lid).lower())) % len(best)]
    if pick.lower() in _LEVI:
        return resolve_item(pick, level, seed, depth + 1)
    return pick


def npc_items(rec):
    """[(count, id)] from NPCO (negative counts are restocking merchant stock), the leveled lists among them
    resolved to an item for PICK_LEVEL."""
    out = []
    for t, d in rec.subs:
        if t == "NPCO":
            item = text(d[4:])
            if item.lower() in _LEVI:
                item = resolve_item(item, PICK_LEVEL, rec.id)
                if item is None:
                    continue
            out.append((struct.unpack_from("<i", d)[0], item))      # (the sign stays: negative is restocking stock)
    return out


def weapon_damage(rec):
    d = rec.get("WPDT")
    return {"chop": [d[22], d[23]], "slash": [d[24], d[25]], "thrust": [d[26], d[27]]}


def weapon_type(w):
    return struct.unpack_from("<h", w.get("WPDT"), 8)[0]


def best_shield(rec, objects):
    best, rating = None, -1
    for _, item in npc_items(rec):
        a = objects.get(item.lower())
        if a is not None and a.tag == "ARMO" and struct.unpack_from("<i", a.get("AODT"))[0] == ARMOR_SHIELD:
            r = struct.unpack_from("<i", a.get("AODT"), 20)[0]
            if r > rating:
                best, rating = a.id, r
    return best


# Equipment slots (OpenMW's InventoryStore::Slot_*), and the ones each ARMO / CLOT type goes in (getEquipmentSlots:
# bracers take the gauntlets' slots, shoes the boots', gloves the gauntlets', a ring either hand)
EQ_HELMET, EQ_CUIRASS, EQ_GREAVES, EQ_LPAULDRON, EQ_RPAULDRON, EQ_LGAUNTLET, EQ_RGAUNTLET, EQ_BOOTS = range(8)
EQ_SHIRT, EQ_PANTS, EQ_SKIRT, EQ_ROBE, EQ_LRING, EQ_RRING, EQ_AMULET, EQ_BELT, EQ_LEFT = range(8, 17)
ARMOR_EQ = [[EQ_HELMET], [EQ_CUIRASS], [EQ_LPAULDRON], [EQ_RPAULDRON], [EQ_GREAVES], [EQ_BOOTS], [EQ_LGAUNTLET],
            [EQ_RGAUNTLET], [EQ_LEFT], [EQ_LGAUNTLET], [EQ_RGAUNTLET]]
CLOTH_EQ = [[EQ_PANTS], [EQ_BOOTS], [EQ_SHIRT], [EQ_BELT], [EQ_ROBE], [EQ_RGAUNTLET], [EQ_LGAUNTLET], [EQ_SKIRT],
            [EQ_LRING, EQ_RRING], [EQ_AMULET]]
PART_HEAD, PART_RFOOT, PART_LFOOT = 0, 15, 16


def item_part_types(rec):
    return [d[0] for t, d in rec.subs if t == "INDX"]


def worn_items(rec, objects, skills, beast):
    """What the NPC wears of what it carries, as OpenMW's InventoryStore::autoEquipArmor picks it: armor before
    clothing (the inventory's type order), each in carried order; armor no better than Unarmored
    ((fUnarmoredBase1 x skill) x (fUnarmoredBase2 x skill)) stays in the pack; a beast can't put on a full helm or
    anything for the feet; a later piece takes a filled slot only if it is armor of the same type with a higher
    skill-adjusted rating, armor of a lower type number (a gauntlet over a bracer), armor over clothing, or clothing
    worth more over clothing. {slot: record}"""
    unarmored = (0.1 * skills[SKILL_UNARMORED]) * (0.065 * skills[SKILL_UNARMORED])
    recs = [objects.get(item.lower()) for _, item in npc_items(rec)]
    armor = [r for r in recs if r is not None and r.tag == "ARMO"]
    cloth = [r for r in recs if r is not None and r.tag == "CLOT"]
    slots = {}

    def rating(r):
        d = r.get("AODT")
        kind, weight, value = struct.unpack_from("<i", d)[0], struct.unpack_from("<f", d, 4)[0], struct.unpack_from("<i", d, 20)[0]
        if weight == 0 or not 0 <= kind < len(ARMOR_BASE_WEIGHT):
            return value
        base = ARMOR_BASE_WEIGHT[kind]
        skill = SKILL_LIGHT if weight <= base * 0.6 + 0.0005 else SKILL_MEDIUM if weight <= base * 0.9 + 0.0005 else SKILL_HEAVY
        return value * skills[skill] / 30.0

    for r in armor + cloth:
        is_armor = r.tag == "ARMO"
        kind = struct.unpack_from("<i", r.get("AODT" if is_armor else "CTDT"))[0]
        table = ARMOR_EQ if is_armor else CLOTH_EQ
        if not 0 <= kind < len(table):
            continue
        if is_armor and rating(r) <= unarmored:
            continue
        if beast and any(p in (PART_HEAD, PART_RFOOT, PART_LFOOT) for p in item_part_types(r)):
            continue
        for slot in table[kind]:
            old = slots.get(slot)
            if old is not None:
                if not is_armor:
                    if old.tag != "CLOT":
                        continue
                    if slot == EQ_LRING and (slots.get(EQ_RRING) is None or
                                             struct.unpack_from("<h", slots[EQ_RRING].get("CTDT"), 8)[0] <=
                                             struct.unpack_from("<h", old.get("CTDT"), 8)[0]):
                        continue
                    if struct.unpack_from("<h", r.get("CTDT"), 8)[0] <= struct.unpack_from("<h", old.get("CTDT"), 8)[0]:
                        continue
                elif old.tag == "ARMO":
                    old_kind = struct.unpack_from("<i", old.get("AODT"))[0]
                    if old_kind == kind:
                        if rating(r) <= rating(old):
                            continue
                    elif old_kind < kind:
                        continue
            slots[slot] = r
            break
    return slots


# Weapon type -> skill
MELEE_SKILL = {0: SKILL_SHORT_BLADE, 1: SKILL_LONG_BLADE, 2: SKILL_LONG_BLADE, 3: SKILL_BLUNT, 4: SKILL_BLUNT,
               5: SKILL_BLUNT, 6: SKILL_SPEAR, 7: SKILL_AXE, 8: SKILL_AXE}


# The weapon skills in the order OpenMW's auto-equip breaks ties in (inventorystore.cpp autoEquipWeapon), and each
# WEAP type's skill
AUTO_EQUIP_SKILLS = [SKILL_LONG_BLADE, SKILL_AXE, SKILL_SPEAR, SKILL_SHORT_BLADE, SKILL_MARKSMAN, SKILL_BLUNT]
WEAPON_SKILL = {**MELEE_SKILL, BOW: SKILL_MARKSMAN, CROSSBOW: SKILL_MARKSMAN, THROWN: SKILL_MARKSMAN}


def combat_weapon(rec, objects, skills):
    """(weapon, ammunition) an actor holds, as OpenMW's auto-equip picks (inventorystore.cpp autoEquipWeapon): the
    weapon skills from the highest (ties in AUTO_EQUIP_SKILLS' order; a skill at 0 never), and for each the carried
    weapon of that skill with the highest maximum damage of chop, slash and thrust (the later of equals). A bow or
    crossbow only with arrows / bolts, the one of the highest chop maximum (the later of equals); without, the next
    skill. Thrown weapons are their own ammunition. (None, None): hand-to-hand."""
    weapons = []
    for _, item in npc_items(rec):
        w = objects.get(item.lower())
        if w is not None and w.tag == "WEAP":
            weapons.append(w)

    def best_of(kind):
        best, top = None, 0
        for w in weapons:
            if weapon_type(w) == kind and w.get("WPDT")[23] >= top:
                best, top = w, w.get("WPDT")[23]
        return best

    arrow, bolt = best_of(ARROW), best_of(BOLT)
    visited = set()
    for _ in AUTO_EQUIP_SKILLS:
        pick, top = None, 0
        for sk in AUTO_EQUIP_SKILLS:
            if sk not in visited and skills[sk] > top:
                pick, top = sk, skills[sk]
        if pick is None:
            break
        visited.add(pick)
        weapon, top = None, 0
        for w in weapons:
            t = weapon_type(w)
            if t in (ARROW, BOLT) or WEAPON_SKILL.get(t) != pick:
                continue
            d = w.get("WPDT")
            for hi in (d[23], d[25], d[27]):          # chop, slash, thrust maximum
                if hi >= top:
                    weapon, top = w, hi
        if weapon is None:
            continue
        t = weapon_type(weapon)
        if t == BOW:
            if arrow is None:
                continue
            return weapon.id, arrow.id
        if t == CROSSBOW:
            if bolt is None:
                continue
            return weapon.id, bolt.id
        return weapon.id, None
    return None, None


# Summonings (102-116, 134) and the wards an actor casts on itself in a fight (OpenMW's spellpriority): the shields,
# Sanctuary, Spell Absorption, Reflect, Fortify Attribute / Health / Fatigue / Attack
SUMMON_EFFECTS = set(range(102, 117)) | {134}
EFFECT_DISPEL = 57
CURE_EFFECTS = {72, 73}             # Cure Poison, Cure Paralyzation
WARD_EFFECTS = {3, 4, 5, 6, 42, 67, 68, 79, 80, 82, 117}


def combat_spells(rec, spel):
    """Spells from the actor's NPCS list that its combat AI casts: harmful ones at a target or by
    touch (of any harmful effect: OpenMW rates every spell an actor has), self-healing, summonings and wards on itself."""
    out = []
    for t, d in rec.subs:
        if t != "NPCS":
            continue
        sid = text(d)
        sp = spel.get(sid.lower())
        if sp is None or struct.unpack_from("<i", sp.get("SPDT"))[0] not in (0, 5):     # spells and powers
            continue
        effects = [struct.unpack_from("<hbbiiiii", e) for tt, e in sp.subs if tt == "ENAM"]
        harm = any(e[0] in HARMFUL_EFFECTS and e[3] in (1, 2) for e in effects)
        heal = any(e[0] in HEAL_EFFECTS and e[3] == 0 for e in effects)
        self_cast = any(e[0] in SUMMON_EFFECTS or (e[0] in WARD_EFFECTS and e[3] == 0) for e in effects)
        # Dispel (on itself, or the foe's wards) and Cure Poison / Paralysis on itself: OpenMW's rateEffect weighs them
        # by what they would take off (spellpriority.cpp: the engine rates them the same)
        cure = any(e[0] == EFFECT_DISPEL or (e[0] in CURE_EFFECTS and e[3] == 0) for e in effects)
        if harm or heal or self_cast or cure:
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
    """(attributes[8], skills[27], health) of an NPC with the Autocalc flag, as OpenMW works them out
    (mwclass/npc.cpp autoCalculateAttributes / autoCalculateSkills):
    - attributes: the race's, +10 for each of the class's two favored ones, then (level - 1) times the sum, over the
      skills that attribute governs, of 1.0 a major skill, 0.5 a minor one and 0.2 any other; at most 100
    - skills: +25 major, +10 minor, then 5 + the race's bonus + 5 in the class's specialization + (level - 1) times
      (1.0 major or minor, else 0.1; +0.5 in the specialization); at most 100
    - health: (strength + endurance) / 2, rounded down, + (level - 1) times 3, +2 for a combat class, +1 for a
      stealth one, +1 when endurance is favored
    OpenMW rounds half to even (round_ieee_754), as Python's round() does."""
    attrs, skills = [40] * 8, [0] * 27
    majors, minors, favored, spec = set(), set(), set(), -1
    if cls is not None:
        d = struct.unpack("<15i", cls.get("CLDT"))
        favored, spec = set(d[0:2]), d[2]
        minors = {d[3 + 2 * i] for i in range(5)}
        majors = {d[4 + 2 * i] for i in range(5)}
    bonus = {}
    if race is not None:
        rd = race.get("RADT")
        for i in range(8):
            attrs[i] = struct.unpack_from("<ii", rd, 56 + 8 * i)[1 if female else 0]
        for i in range(7):
            skill, b = struct.unpack_from("<ii", rd, 8 * i)
            if 0 <= skill < 27:
                bonus[skill] = b
    # (each skill's governing attribute and specialization, from the SKIL records; the vanilla table when unbound)
    table = _SKILLS or {sk: (SKILL_ATTRIBUTE[sk], next(sp for sp, ss in SPEC_SKILLS.items() if sk in ss))
                        for sk in range(27)}
    growth = level - 1
    for i in range(8):
        attrs[i] += 10 if i in favored else 0
        per_level = sum(1.0 if sk in majors else 0.5 if sk in minors else 0.2
                        for sk, (attr, _) in table.items() if attr == i)
        attrs[i] = min(round(attrs[i] + growth * per_level), 100)
    for s in range(27):
        skills[s] += 25 if s in majors else 10 if s in minors else 0
        same_spec = table[s][1] == spec
        per_level = (1.0 if s in majors or s in minors else 0.1) + (0.5 if same_spec else 0.0)
        skills[s] = min(round(skills[s] + 5 + bonus.get(s, 0) + (5 if same_spec else 0) + growth * per_level), 100)
    mult = 3 + (2 if spec == 0 else 1 if spec == 2 else 0) + (1 if ATTR_ENDURANCE in favored else 0)
    health = (attrs[ATTR_STRENGTH] + attrs[ATTR_ENDURANCE]) // 2 + mult * growth
    return attrs, skills, health


# Skills of each specialization (combat, magic, stealth), and each skill's governing attribute (vanilla SKIL records)
SPEC_SKILLS = {0: (0, 1, 2, 3, 4, 5, 6, 7, 8), 1: (9, 10, 11, 12, 13, 14, 15, 16, 17),
               2: (18, 19, 20, 21, 22, 23, 24, 25, 26)}
SKILL_ATTRIBUTE = [3, 0, 5, 5, 0, 0, 0, 5, 4, 1, 2, 2, 6, 1, 2, 2, 1, 4, 1, 3, 0, 3, 4, 3, 6, 6, 4]


EFFECT_FORTIFY_MAX_MAGICKA = 84
# SPDT types whose effects are always on the actor: ability, blight, disease, curse
LASTING_SPELL_TYPES = (1, 2, 3, 4)


def fortify_max_magicka(rec, db):
    """The Fortify Maximum Magicka (magnitude, ENAM's minimum) of an NPC's own lasting spells and its race's."""
    ids = [text(d).lower() for t, d in rec.subs if t == "NPCS"]
    race = db["RACE"].get((rec.zstr("RNAM") or "").lower())
    if race is not None:
        ids += [text(d).lower() for t, d in race.subs if t == "NPCS"]
    total = 0
    for sid in ids:
        sp = db["SPEL"].get(sid)
        if sp is None or struct.unpack_from("<i", sp.get("SPDT"))[0] not in LASTING_SPELL_TYPES:
            continue
        for t, d in sp.subs:
            if t == "ENAM" and struct.unpack_from("<h", d)[0] == EFFECT_FORTIFY_MAX_MAGICKA:
                total += struct.unpack_from("<i", d, 16)[0]
    return total


def travel_destinations(rec):
    """Caravaners, boatmen, guild guides: [(pos[3], rot[3], interior cell name or None)] from DODT / DNAM."""
    out = []
    for t, d in rec.subs:
        if t == "DODT":
            v = struct.unpack("<6f", d)
            out.append([list(v[:3]), list(v[3:]), None])
        elif t == "DNAM" and out:
            out[-1][2] = text(d)
    return out


def preferred_shield(rec, db, skills):
    """The shield it takes up: the one autoEquip puts on it (OpenMW's getPreferredShield)."""
    race = db["RACE"].get((rec.zstr("RNAM") or "").lower())
    beast = race is not None and struct.unpack_from("<i", race.get("RADT"), 136)[0] & 2
    shield = worn_items(rec, db["objects"], skills, beast).get(EQ_LEFT)
    return shield.id if shield is not None else ""


def npc_stats(rec, db):
    bind(db)
    npdt = rec.get("NPDT")
    female = struct.unpack("<i", rec.get("FLAG"))[0] & 1
    if len(npdt) == 12:
        level = struct.unpack_from("<h", npdt)[0]
        gold = struct.unpack_from("<i", npdt, 8)[0]
        attrs, skills, health = autocalc(level, db["RACE"].get((rec.zstr("RNAM") or "").lower()),
                                         db["CLAS"].get((rec.zstr("CNAM") or "").lower()), female)
        magicka = None
    else:
        attrs, skills = list(npdt[2:10]), list(npdt[10:37])
        health, magicka, stored_fatigue = struct.unpack_from("<3H", npdt, 38)
        gold = struct.unpack_from("<i", npdt, 48)[0]
    # magicka: intelligence x (fNPCbaseMagickaMult + a tenth of the Fortify Maximum Magicka on it), as OpenMW's
    # CreatureStats::recalculateMagicka. It runs for every autocalc NPC, and for one with stored stats only when such a
    # fortify is on it (a Breton's or High Elf's birthright): else the stored magicka stays. The engine doesn't apply
    # Fortify Maximum Magicka to NPCs (build_game.py CONSTANT_TARGET_EFFECTS), so it is counted here
    fortify = fortify_max_magicka(rec, db)
    if magicka is None or fortify:
        magicka = int(attrs[1] * (_MAGICKA_MULT + 0.1 * fortify))
    # fatigue: strength + willpower + agility + endurance for an autocalc NPC (OpenMW's CreatureStats::setAttribute sets
    # it whenever one of those four is set); one with stored stats keeps the record's, which Npc::ensureCustomData sets
    # after the attributes. An effect on one of the four makes it the sum later (the engine: actorSpellStats). The base
    # game's editor stored the sum for most, the add-ons' did not: Tharsten Heart-Fang 800, sum 391
    if len(npdt) == 12:
        fatigue = attrs[ATTR_STRENGTH] + attrs[ATTR_WILLPOWER] + attrs[ATTR_AGILITY] + attrs[ATTR_ENDURANCE]
    else:
        fatigue = stored_fatigue
    aidt = rec.get("AIDT")
    fight = aidt[2] if aidt else 30
    flee = aidt[3] if aidt else 30
    services = struct.unpack_from("<I", aidt, 8)[0] if aidt and len(aidt) >= 12 else 0
    weapon, ammo = combat_weapon(rec, db["objects"], skills)
    return {"attributes": attrs, "skills": skills, "health": max(int(health), 0), "fatigue": max(int(fatigue), 1),
            "magicka": magicka, "gold": max(gold, 0), "fight": fight, "flee": flee, "services": services,
            "weapon": weapon or "", "ammo": ammo or "", "shield": preferred_shield(rec, db, skills),
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
            target = text(d[14:46]).lower()
            return [3 if tag == "AI_F" else 2, target, x, y, z, dur]
        if tag == "AI_A":
            return [4, text(d[:32]).lower(), 0, 0, 0, 0]
    return None
