"""What an actor carries from its leveled item lists, and the looks of an NPC whose lists dress them.

OpenMW rolls an actor's leveled item lists (LEVI) at the player's level when the actor is made (getLevelledItem) and
auto-equips what came. The 3DS draws people from libraries the converter dressed (npc.py), so:
- a list line that can put visible clothes or armour on an NPC (a "look" line) is picked here, the same way
  npcstats.resolve_item does, once for each level step of its lists: each step whose clothes differ gets a library of
  its own (cells/actors/<xx>/<id>_<level>.aml), and the engine takes the step at or under the player's level when it
  fills the NPC's pack (source/mwworld/cellstore.cpp fillContents, ActorDef::looks);
- every other list line (weapons, shields, rings, scrolls, a creature's whole pack) stays a list in game.json, and the
  engine rolls it at the player's level, so each actor made from the record gets its own (two summoned Golden Saints
  can hold different weapons). What it fights with is then picked from its pack at run time (actorAutoEquip) and drawn
  from the third-person pieces where its library's differ (npcGear).

Not in the cell cache's code hash (convert_cell.py): the cells and their libraries stay the same, the look libraries
are written by level.py after the cells.

Usage (from level.py): runtime_items(rec, db), look_tiers(rec, db), look_library(conv, rec, level)
"""
import struct

import npcstats
from meshes import FLAG_BLEND, FLAG_TEST
from npcstats import EQ_LEFT, npc_stats, text, worn_items

CLOT_RING, CLOT_AMULET = 8, 9
ARMO_SHIELD = 8


def shows(rec):
    """A piece of clothing or armour that puts something on the body: not a ring, an amulet or a shield."""
    if rec is None or rec.tag not in ("CLOT", "ARMO"):
        return False
    kind = struct.unpack_from("<i", rec.get("CTDT" if rec.tag == "CLOT" else "AODT"))[0]
    return kind not in (CLOT_RING, CLOT_AMULET) if rec.tag == "CLOT" else kind != ARMO_SHIELD


def is_look_list(lid, db, depth=0):
    """A leveled item list that can give something that shows when worn (nested lists followed)."""
    entries = db["LEVI"].get(lid.lower())
    if not entries or depth > 5:
        return False
    for _, iid in entries:
        if iid.lower() in db["LEVI"]:
            if is_look_list(iid, db, depth + 1):
                return True
        elif shows(db["objects"].get(iid.lower())):
            return True
    return False


def _lines(rec):
    for t, d in rec.subs:
        if t == "NPCO":
            yield struct.unpack_from("<i", d)[0], text(d[4:])


def look_lines(rec, db):
    """An NPC's pack lines whose leveled list dresses them: [(count, list id)]. None for a creature."""
    if rec.tag != "NPC_":
        return []
    return [(n, i) for n, i in _lines(rec) if i.lower() in db["LEVI"] and is_look_list(i, db)]


def runtime_items(rec, db):
    """[(count, id)] of an actor's pack for game.json: its own items, and its leveled lists as lists (the engine rolls
    them), but for the look lines of an NPC, which are its looks' (look_tiers) or, with one look, picked here."""
    looks = {i.lower() for _, i in look_lines(rec, db)}
    out = []
    for n, item in _lines(rec):
        if item.lower() in looks:
            continue
        out.append((n, item.lower() if item.lower() in db["LEVI"] else item))
    if looks and len(look_tiers(rec, db)) < 2:
        out += _look_items(rec, db, 1)
    return out


def _look_items(rec, db, level):
    out = []
    for n, lid in look_lines(rec, db):
        pick = npcstats.resolve_item(lid, level, rec.id)
        if pick is not None:
            out.append((n, pick))
    return out


def _list_levels(lid, db, depth=0):
    levels = set()
    for lvl, iid in db["LEVI"].get(lid.lower(), []):
        levels.add(lvl)
        if iid.lower() in db["LEVI"] and depth < 5:
            levels |= _list_levels(iid, db, depth + 1)
    return levels


_tiers = {}


def look_tiers(rec, db):
    """The level steps at which an NPC's look lines give it something else: [(level, [(count, id)])], the first at
    level 1. One entry for an NPC without look lines, or whose lines give the same at every level."""
    key = rec.id.lower()
    if key in _tiers:
        return _tiers[key]
    npcstats.bind(db)
    lines = look_lines(rec, db)
    tiers = [(1, _look_items(rec, db, 1))]
    if lines:
        levels = set()
        for _, lid in lines:
            levels |= _list_levels(lid, db)
        for lvl in sorted(l for l in levels if l > 1):
            items = _look_items(rec, db, lvl)
            if items != tiers[-1][1]:
                tiers.append((lvl, items))
    _tiers[key] = tiers
    return tiers


def dressed(rec, db, level):
    """(worn ids that show, weapon, shield) the NPC's library holds when its lists are picked for that level."""
    keep = npcstats.PICK_LEVEL
    npcstats.PICK_LEVEL = level
    try:
        race = db["RACE"].get((rec.zstr("RNAM") or "").lower())
        beast = race is not None and struct.unpack_from("<i", race.get("RADT"), 136)[0] & 2
        stats = npc_stats(rec, db)
        worn = worn_items(rec, db["objects"], stats["skills"], beast)
        ids = [r.id.lower() for slot, r in worn.items() if r is not None and r.tag in ("ARMO", "CLOT") and slot != EQ_LEFT]
        return ids, (stats["weapon"] or "").lower(), (stats["shield"] or "").lower()
    finally:
        npcstats.PICK_LEVEL = keep


def visible(ids, db):
    return sorted(i for i in ids if shows(db["objects"].get(i)))


def look_library(conv, rec, level):
    """cells/actors/<xx>/<id>_<level>.aml: the NPC dressed with its lists picked for that level (as spawn_library
    dresses it for level 1). Its path."""
    keep = npcstats.PICK_LEVEL
    npcstats.PICK_LEVEL = level
    try:
        ref = {"id": rec.id, "pos": (0.0, 0.0, 0.0), "rot": (0.0, 0.0, 0.0), "scale": 1.0}
        shapes, info = conv.npcs.build(rec, ref)
        if not info:
            return None
        sk = conv.skeletons[conv.skeleton(info["skeleton"])]
        for s in shapes:
            if s.get("tex") and "raw" in s:
                conv.textures.request_shape(s, s["flags"] & (FLAG_BLEND | FLAG_TEST))
        return conv.actor_library(f"{rec.id.lower()}@{level}", shapes, sk)
    finally:
        npcstats.PICK_LEVEL = keep


def looks_entry(conv, rec, db):
    """game.json's "looks" for an NPC with more than one ([] otherwise): the first a level 1 player's (its items only:
    the record's library, worn pieces, weapon and shield are the first look's), then each step with the library it is
    drawn from, that library's worn pieces, weapon and shield. A step whose clothes show as the one before keeps that
    one's library (and says nothing of it when that is the record's own)."""
    tiers = look_tiers(rec, db)
    if len(tiers) < 2:
        return []
    out = [{"level": 1, "items": [[n, i.lower()] for n, i in tiers[0][1]]}]
    worn, _, _ = dressed(rec, db, 1)
    drawn = {"shown": visible(worn, db)}           # the library the step before is drawn from (the record's: no lib)
    for lvl, items in tiers[1:]:
        entry = {"level": lvl, "items": [[n, i.lower()] for n, i in items]}
        worn, weapon, shield = dressed(rec, db, lvl)
        shown = visible(worn, db)
        if shown != drawn["shown"]:
            lib = look_library(conv, rec, lvl)
            if lib is not None:
                drawn = {"shown": shown, "lib": lib, "worn": worn, "weapon": weapon, "shield": shield}
        entry.update({k: v for k, v in drawn.items() if k != "shown"})
        out.append(entry)
    return out

