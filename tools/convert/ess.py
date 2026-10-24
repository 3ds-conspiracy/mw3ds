"""Converts Morrowind's own saves (.ess, from the PC game's Saves folder) into the engine's save format.

    python tools/convert/ess.py <save.ess> [more.ess ...] --data out/world --out <folder>

Writes <folder>/3ds/mw3ds/saves/pc_<save name>.json and its .meta: copy the 3ds folder to the root of the SD card
and the saves show in the game's Saves list (they merge with the saves already there; none is overwritten, as the
game's own are save_NNN.json).

What comes across: the character (name, race, class with a custom one, birthsign, sex, head, hair, level and its
progress, attributes, skills and their progress, health / magicka / fatigue, spells with the ones the player made,
bounty, factions with rank, reputation and expulsion), the inventory with what is worn and potions brewed, where the
player stands, the time of day and the calendar, the globals, the topics known, the journal in its order and the
quests' stages, the Mark, the kill counts, and what changed in the world: objects taken, moved, disabled, unlocked
and disarmed, the dead, people's and containers' contents, dispositions, items put down, and the scripts'
variables (global scripts, objects', carried items'). Objects are found by Morrowind's reference numbers
(esscells.py). Not: creatures rolled from a levelled list (the engine rolls its own at the spawn point), items
enchanted by the player, running spell effects, the map's fog.

Only Morrowind.esm is converted, so a save made with Tribunal, Bloodmoon or other plugins loses whatever came
from them (items, spells, quests); each one dropped is listed.

The record layout follows OpenMW's essimporter (apps/essimporter), read for the format, written here anew.
"""
import argparse
import json
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esscells
import mwfiles
from mwfiles import read_records, text

CELL_SIZE = 8192
# 1900-01-01 to 1970-01-01, in ms: the console's osGetTime counts from 1900 (savegamedialog.cpp formatTime)
EPOCH_1900_MS = 2208988800 * 1000
# The ids the save uses for the player's own records
PLAYER_NPC = "player"
PLAYER_REF = "PlayerSaveGame"
CUSTOM_CLASS = "NEWCLASSID_CHARGEN"


def read_game_json(path):
    raw = path.read_bytes()
    return json.loads(zlib.decompress(raw[8:]) if raw[:4] == b"MWZ1" else raw)


def game_tables(data):
    """What the conversion checks the save against, from the converted data (out/world)."""
    races = {r["id"].lower(): r for r in read_game_json(data / "game_races.json")["races"]}
    classes = {c["id"].lower(): c for c in read_game_json(data / "game_classes.json")["classes"]}
    signs = {b["id"].lower(): b for b in read_game_json(data / "game_birthsigns.json")["birthsigns"]}
    journal = {}
    for p in sorted(data.glob("game_journal_*.json")):
        journal.update(read_game_json(p)["journal"])
    objects = {}                     # id -> (record type, whether it wears out: weapons, armour keep a condition,
    health = {}                      #   tools their uses left), and id -> that condition when new
    for p in sorted(data.glob("game_objects_*.json")):
        for k, o in read_game_json(p)["objects"].items():
            full = o.get("health", o.get("uses"))
            objects[k.lower()] = (o.get("type", ""), full is not None)
            if full is not None:
                health[k.lower()] = int(full)
    spells = {k.lower() for k in read_game_json(data / "game_spells.json")["spells"]}
    skills = [s.get("specialization", -1) for s in read_game_json(data / "game.json")["skills"]]
    cells = {p.stem for p in (data / "cells").glob("*/*.cel")}
    globals_ = {k.lower() for k in read_game_json(data / "game.json").get("globals", {})}
    return dict(races=races, classes=classes, signs=signs, journal=journal, objects=objects, health=health,
                spells=spells, skills=skills, cells=cells, globals=globals_)


def journal_infos(esm):
    """Journal INFO id -> (quest id, stage), from Morrowind.esm's journal topics (DIAL type 4)."""
    out, quest = {}, None
    for r in read_records(esm, {"DIAL", "INFO"}):
        if r.tag == "DIAL":
            d = r.get("DATA")
            quest = r.id if d and d[0] == 4 else None
        elif quest is not None:
            info, data = r.zstr("INAM"), r.get("DATA")
            if info and data and len(data) >= 8:
                out[info] = (quest, struct.unpack_from("<i", data, 4)[0])
    return out


def subs(r, tag):
    return [v for t, v in r.subs if t == tag]


def zstr(b):
    return text(b.split(b"\0", 1)[0])


def creation_stats(race, cls, female, skill_spec):
    """Attributes and skills as character creation makes them (World::recomputeStats): the saves keep what the
    player gained on top, so this is taken off what the PC save holds."""
    attrs = [(race["attributes"][i][1 if female else 0] if race else 40) + (10 if cls and i in cls["attributes"] else 0)
             for i in range(8)]
    skills = []
    for s in range(27):
        v = 5
        if cls:
            v += 25 * cls["major"].count(s) + 10 * cls["minor"].count(s)
            if s < len(skill_spec) and skill_spec[s] == cls["specialization"]:
                v += 5
        skills.append(v)
    for s, b in (race["skill_bonus"] if race else []):
        if 0 <= s < 27:
            skills[s] += b
    return attrs, skills


def parse_inventory(r):
    """NPCO stacks (count, id) in order, with their item data and script, and the WIDX list of worn ones (index into
    NPCO)."""
    items, worn, cur = [], set(), None
    for k, (t, v) in enumerate(r.subs):
        if t == "NPCO":
            n = struct.unpack_from("<i", v)[0]
            cur = {"id": zstr(v[4:36]), "count": abs(n), "signed": n, "charge": -1.0, "condition": -1,
                   "soul": "", "script": None}
            items.append(cur)
        elif cur is not None and t == "SCRI":
            cur["script"] = esscells.read_scri(script_subs(r.subs, k))
        elif cur is not None and t == "XHLT":
            cur["condition"] = struct.unpack_from("<i", v)[0]
        elif cur is not None and t == "XCHG":
            cur["charge"] = struct.unpack_from("<f", v)[0]
        elif cur is not None and t == "XSOL":
            cur["soul"] = zstr(v).lower()
        elif t == "WIDX":
            worn.add(struct.unpack_from("<i", v)[0])
    return items, worn


def exterior_name(x, y):
    gx, gy = int(x // CELL_SIZE), int(y // CELL_SIZE)
    return "ext_%s%d_%s%d" % ("m" if gx < 0 else "", abs(gx), "m" if gy < 0 else "", abs(gy))


def convert(ess, tables, infos, say):
    recs = list(read_records(ess))
    by = {}
    for r in recs:
        by.setdefault(r.tag, []).append(r)
    head = recs[0]
    if head.tag != "TES3":
        raise ValueError("not a Morrowind save")
    masters = [zstr(v) for v in subs(head, "MAST")]
    extra = [m for m in masters if m.lower() != "morrowind.esm"]
    if extra:
        say(f"  made with {', '.join(extra)}: only what comes from Morrowind.esm comes across")
    dropped = []

    npc = next(r for r in by.get("NPC_", []) if r.id == PLAYER_NPC)
    name = zstr(npc.get("FNAM"))
    race_id, cls_id = zstr(npc.get("RNAM")), zstr(npc.get("CNAM"))
    female = bool(struct.unpack_from("<I", npc.get("FLAG"))[0] & 1)
    npdt = npc.get("NPDT")
    level = struct.unpack_from("<h", npdt)[0]
    base_attrs, base_skills = list(npdt[2:10]), list(npdt[10:37])

    race = tables["races"].get(race_id.lower())
    if not race:
        raise ValueError(f"race {race_id} isn't in the converted game")
    stats = {"name": name, "race": race["id"], "class": cls_id, "female": int(female),
             "head": zstr(npc.get("BNAM")), "hair": zstr(npc.get("KNAM")), "level": level}
    cls = tables["classes"].get(cls_id.lower())
    if cls_id == CUSTOM_CLASS:
        rec = next((r for r in by.get("CLAS", []) if r.id == CUSTOM_CLASS), None)
        if rec:
            c = struct.unpack_from("<2i i 10i", rec.get("CLDT"))
            cls = {"name": zstr(rec.get("FNAM")), "desc": zstr(rec.get("DESC") or b""), "attributes": list(c[0:2]),
                   "specialization": c[2], "minor": [c[3 + 2 * k] for k in range(5)],
                   "major": [c[4 + 2 * k] for k in range(5)]}
            stats["custom_class"] = {k: cls[k] for k in ("name", "desc", "specialization", "attributes", "major",
                                                         "minor")}
            stats["class"] = "NEWCLASSID_CHARGEN"
    if not cls:
        raise ValueError(f"class {cls_id} isn't in the converted game")
    born_attrs, born_skills = creation_stats(race, cls, female, tables["skills"])

    pc = by["PCDT"][0]
    pnam = pc.get("PNAM")
    flags, level_progress = struct.unpack_from("<iI", pnam)
    stats["level_progress"] = level_progress
    stats["skill_progress"] = [round(v, 4) for v in struct.unpack_from("<27f", pnam, 8)]
    stats["attr_ups"] = list(pnam[116:124])
    sign = pc.zstr("BNAM") or ""
    stats["birthsign"] = next((b["id"] for k, b in tables["signs"].items() if k == sign.lower()), sign)

    # the player's reference: where they stand, health / fatigue / magicka (current, base), skills now
    ref = next(r for r in by.get("REFR", []) if zstr(r.get("NAME") or b"") == PLAYER_REF)
    pos = struct.unpack_from("<6f", ref.get("DATA"))
    acdt = ref.get("ACDT")
    dyn = struct.unpack_from("<6f", acdt, 40)        # health, fatigue, magicka: (current, base) each
    stats["bars"] = [round(dyn[0], 2), round(dyn[1], 2), round(dyn[4], 2), round(dyn[5], 2), round(dyn[2], 2),
                     round(dyn[3], 2)]
    cur_attrs = struct.unpack_from("<16f", acdt, 80)
    stats["attr_damage"] = [max(0.0, round(cur_attrs[2 * i + 1] - cur_attrs[2 * i], 2)) for i in range(8)]
    stats["health_bonus"] = 0
    # The base values: the reference's own (ACDT's attributes, CHRD's skills, whole numbers of any size) over the
    # NPC record's bytes, which stop at 255
    base_attrs = [int(round(cur_attrs[2 * i + 1])) for i in range(8)]
    chrd = ref.get("CHRD")
    if chrd and len(chrd) >= 216:
        skills_now = struct.unpack_from("<54i", chrd)
        base_skills = [skills_now[2 * s] for s in range(27)]
        stats["skill_damage"] = [max(0, skills_now[2 * s] - skills_now[2 * s + 1]) for s in range(27)]
    stats["attr_bonus"] = [base_attrs[i] - born_attrs[i] for i in range(8)]
    stats["skill_bonus"] = [base_skills[s] - born_skills[s] for s in range(27)]

    made_spells, potions = made_records(by, tables)
    known = tables["spells"] | {m["id"] for m in made_spells}

    # spells the player knows (not race or birthsign ones: the engine adds those)
    given = {s.lower() for s in race.get("spells", [])}
    given |= {s.lower() for s in tables["signs"].get(sign.lower(), {}).get("spells", [])}
    spells = []
    for v in subs(npc, "NPCS"):
        sid = zstr(v).lower()
        if sid in given:
            continue
        if sid in known:
            spells.append(sid)
        else:
            dropped.append("spell " + sid)
    stats["spells"] = spells
    stats["selected_spell"] = ""
    stats["books_read"] = []
    stats["quick_keys"] = ["", "", "", ""]
    stats["powers_used"] = {}
    stats.setdefault("skill_damage", [0] * 27)

    # the inventory (NPCC of the player): its stacks, and what is worn
    npcc = next(r for r in by.get("NPCC", []) if r.id == PLAYER_REF)
    items, worn = parse_inventory(npcc)
    inventory = []
    for i, it in enumerate(items):
        iid = it["id"].lower()
        if iid not in tables["objects"] and iid not in {p["id"] for p in potions}:
            dropped.append(f"item {it['id']} x{it['count']}")
            continue
        o = {"id": iid, "count": it["count"], "equipped": int(i in worn)}
        # (a condition at the item's full health is one never worn down: the 3DS keeps no condition for it)
        if (it["condition"] >= 0 and tables["objects"].get(iid, ("", False))[1]
                and it["condition"] != tables["health"].get(iid)):
            o["condition"] = it["condition"]
        if it["soul"]:
            o["soul"] = it["soul"]
        if it["charge"] >= 0:
            o["charge"] = it["charge"]
        inventory.append(o)

    # where: an interior by its name, else the exterior grid cell under the player
    cell_name = zstr(by["GAME"][0].get("GMDT")[:64])
    interior = mwfiles.safe_name(cell_name)
    cell = interior if interior in tables["cells"] and not interior.startswith("ext_") else exterior_name(pos[0], pos[1])
    if cell not in tables["cells"]:
        raise ValueError(f"the player's cell ({cell_name}) isn't in the converted game")

    # globals: GameHour, the calendar, quest and script globals
    glob, hour = {}, 9.0
    for r in by.get("GLOB", []):
        kind, gid = (r.zstr("FNAM") or "f"), r.id.lower()
        v = struct.unpack_from("<f", r.get("FLTV"))[0]
        glob[gid] = v if kind == "f" else int(round(v))
        if gid == "gamehour":
            hour = v
    # the engine counts the hours since the game began (day 1): DaysPassed's whole days and the hour of this one
    if "dayspassed" in glob:
        hour += 24 * max(0, int(glob["dayspassed"]) - 1)

    # topics known, the journal (its entries in order, by INFO id) and the quests' stages
    topics = sorted({zstr(v).lower() for v in subs(pc, "DNAM")})
    stage = {}
    for r in by.get("DIAL", []):
        x = r.get("XIDX")
        if x:
            i = struct.unpack_from("<i", x)[0]
            if i > 0:
                stage[r.id.lower()] = i
    journal, finished = [], set()
    jour = text(by["JOUR"][0].get("NAME")).replace("@", "").replace("#", "") if by.get("JOUR") else ""
    for r in by.get("QUES", []):
        for v in subs(r, "DATA"):
            q = infos.get(zstr(v))
            if not q:
                continue
            qid = q[0].lower()
            quest = tables["journal"].get(qid)
            entry = next((e for e in quest["entries"] if e["index"] == q[1]), None) if quest else None
            if not entry:
                dropped.append(f"journal {q[0]} {q[1]}")
                continue
            journal.append({"quest": qid, "index": q[1], "text": entry["text"]})
            # (as the engine's journal does it when an entry is written: a finishing one ends the quest, a
            # restarting one opens it again)
            if entry.get("finished"):
                finished.add(qid)
            elif entry.get("restart"):
                finished.discard(qid)

    if jour:
        journal.sort(key=lambda j: (jour.find(j["text"]) if jour.find(j["text"]) >= 0 else len(jour)))

    # factions: rank, reputation, expelled (PCDT FNAM, 44 bytes each)
    ranks, rep, expelled = {}, {}, []
    for v in subs(pc, "FNAM"):
        rank, reputation, fl = v[0], struct.unpack_from("<i", v, 4)[0], v[8]
        fid = zstr(v[12:44]).lower()
        if fl & 1 or rank:                   # (0x1: a member; a faction known only by its reputation has neither)
            ranks[fid] = rank
        if reputation or fid not in ranks:
            rep[fid] = reputation
        if fl & 2:
            expelled.append(fid)
    bounty = 0
    for t, v in pc.subs:
        if t == "CNAM" and len(v) == 4:
            bounty = struct.unpack_from("<i", v)[0]

    save = {
        "version": 1, "cell": cell, "feet": [round(p, 2) for p in pos[:3]], "yaw": round(pos[5], 4), "pitch": 0,
        "time": 0, "hour": round(hour, 4), "controls": int(not flags & 0x4), "jumping": int(not flags & 0x1000),
        "fighting": 1, "magic": 1, "controlsoff": 0, "menus": 0xFFFF, "stats": stats, "bounty": bounty,
        "no_teleport": bool(flags & 0x40000), "inventory": inventory, "journal": journal,
        "journal_index": stage, "quests_finished": sorted(finished), "topics": topics, "globals": glob,
        "faction_ranks": ranks, "faction_rep": rep, "expelled": expelled, "refs": [], "scripts": [],
    }
    mnam = pc.zstr("MNAM")
    if mnam:
        mx, my, mz, mrot, cx, cy = struct.unpack_from("<4f2i", pnam, 144)
        mcell = mwfiles.safe_name(mnam)
        mcell = mcell if (cx, cy) == (0, 0) and mcell in tables["cells"] else exterior_name(mx, my)
        if mcell in tables["cells"]:
            save["mark"] = {"cell": mcell, "pos": [mx, my, mz], "yaw": mrot}
    save["made_spells"], save["potions"] = made_spells, potions
    save["refs"], save["scripts"], save["dropped"] = world_changes(by, masters, tables, dropped)
    for it in items:
        if it["script"] and it["id"].lower() in tables["objects"]:
            sc = script_state(tables, it["script"], dropped)
            if sc:
                save["scripts"].append(dict(sc, item=it["id"].lower()))
    kills = {}
    for r in by.get("KLST", []):
        cur = None
        for t, v in r.subs:
            if t == "KNAM":
                cur = zstr(v).lower()
            elif t == "CNAM" and cur:
                kills[cur] = struct.unpack_from("<i", v)[0]
    save["dead_counts"] = kills
    return save, dropped, cell_name


def effects(r):
    """A SPEL / ALCH record's effects (ENAM): (effect, skill, attribute, range, area, duration, min, max) each."""
    return [struct.unpack("<hbbiiiii", v) for t, v in r.subs if t == "ENAM"]


def made_records(by, tables):
    """Spells the player made (SPEL) and potions brewed (ALCH) that Morrowind.esm doesn't have, as the engine keeps
    them (made_spells, potions)."""
    spells, potions = [], []
    for r in by.get("SPEL", []):
        sid = r.id.lower()
        if sid in tables["spells"]:
            continue
        kind, cost, _ = struct.unpack_from("<3i", r.get("SPDT"))
        spells.append({"id": sid, "name": r.zstr("FNAM") or r.id, "cost": cost,
                       "effects": [[e[0], e[1], e[2], e[6], e[7], e[5], e[3]] for e in effects(r)]})
    for r in by.get("ALCH", []):
        pid = r.id.lower()
        if pid in tables["objects"]:
            continue
        weight, value, _ = struct.unpack_from("<fii", r.get("ALDT"))
        potions.append({"id": pid, "name": r.zstr("FNAM") or r.id, "value": value, "weight": round(weight, 3),
                        "icon": r.zstr("TEXT") or "", "icon_ix": -1,
                        "effects": [[e[0], e[1], e[2], e[6], e[5]] for e in effects(r)]})
    return spells, potions


def script_subs(subs, k):
    """The SCRI at subs[k] and the local values that follow it."""
    end = next((j for j in range(k + 1, len(subs)) if subs[j][0] not in ("SLCS", "SLSD", "SLLD", "SLFD")), len(subs))
    return subs[k:end]


def script_state(tables, scri, dropped):
    """A script's locals as the engine keeps them, from Morrowind's (script, shorts, longs, floats)."""
    name, shorts, longs, floats = scri
    sc = tables["scripts"].get((name or "").lower())
    if not sc:
        if name:
            dropped.append("script " + name)
        return None
    return {"name": sc["name"], "item": "", "running": 1,
            "locals": [round(v, 4) for v in esscells.locals_from_mw(sc, shorts, longs, floats)]}


def item_list(r, tables):
    """A container's or actor's items as the engine keeps them: [count, id] or [count, id, condition, soul, charge]."""
    out = []
    for it in parse_inventory(r)[0]:
        wears = tables["objects"].get(it["id"].lower(), ("", False))[1]
        cond = it["condition"] if wears else -1
        # (a negative count: a merchant's stock that comes back, as the engine keeps it too)
        if cond >= 0 or it["soul"] or it["charge"] >= 0:
            out.append([it["signed"], it["id"].lower(), cond, it["soul"], it["charge"]])
        else:
            out.append([it["signed"], it["id"].lower()])
    return out


def world_changes(by, masters, tables, dropped):
    """What changed in the world (CELL records' references, their actors and containers, global scripts): the
    engine's refs, scripts and dropped items."""
    world = tables["world"]
    mw = next((k + 1 for k, m in enumerate(masters) if m.lower() == "morrowind.esm"), 1)

    def indexed(tag):
        out = {}
        for r in by.get(tag, []):
            ix = r.get("NPDT")[4:8] if tag == "NPCC" else r.get("INDX")
            out[(r.id.lower(), struct.unpack_from("<i", ix)[0] if ix else 0)] = r
        return out
    npcc, crec, cntc = indexed("NPCC"), indexed("CREC"), indexed("CNTC")
    refs, scripts, drops, missed = [], [], [], 0
    for cell in by.get("CELL", []):
        flags, gx, gy = struct.unpack_from("<Iii", cell.get("DATA"))
        file = esscells.cell_file(cell.id or "", flags, gx, gy)
        if file not in world.paths:
            continue
        to_ours, _ = world.link(file)
        ours = world.ours(file)
        for g in esscells.ref_groups(cell.subs):
            name = text(esscells.sub(g, "NAME") or b"")
            # actors and containers with their own records are named <id><8 digits>, the digits their INDX
            indexed = len(name) > 8 and name[-8:].isdigit()
            base, n = (name[:-8], int(name[-8:])) if indexed else (name, -1)
            dat = esscells.sub(g, "DATA")
            pos = struct.unpack("<6f", dat) if dat else None
            if g[0] >> 24 == 0:
                # made in the game: an item put down is kept, as the engine's dropped items
                o = tables["objects"].get(base.lower())
                if o and o[0] in esscells.ITEM_TYPES and pos and esscells.sub(g, "DELE") is None:
                    cnt = esscells.sub(g, "NAM9")
                    drops.append({"id": base.lower(), "n": struct.unpack("<i", cnt)[0] if cnt else 1, "c": file,
                                  "pos": [round(v, 2) for v in pos[:3]], "yaw": round(pos[5], 4)})
                continue
            if g[0] >> 24 != mw:
                continue
            i = to_ours.get(g[0] & 0xFFFFFF)
            if i is None:
                missed += 1
                continue
            orig = world.cells[file]["refs"][g[0] & 0xFFFFFF]
            if orig["id"] in world.leveled:
                continue                     # (a spawn point: the engine rolls its own creature there)
            e = {"c": file, "i": i, "id": ours[i][0]}
            z = esscells.sub(g, "ZNAM")
            if z is not None and z[0] == 0:
                e["enabled"] = 0
            if esscells.sub(g, "DELE") is not None:
                # (an item picked up, a body cleared away; anything else gone from the world reads as disabled)
                kind = tables["objects"].get(orig["id"], ("",))[0]
                if kind in esscells.ITEM_TYPES:
                    e["taken"] = 1
                elif kind in ("NPC_", "CREA"):
                    e["dead"], e["taken"] = 1, 1
                else:
                    e["enabled"] = 0
                if orig["lock"] > 0:
                    e["lock"] = orig["lock"]
            else:
                # (a lock below zero: unlocked, its level kept for Lock spells; both games keep it so)
                lock = esscells.sub(g, "FLTV")
                if lock is not None and struct.unpack("<i", lock)[0] != 0:
                    e["lock"] = struct.unpack("<i", lock)[0]
                if orig["trap"] and esscells.sub(g, "TNAM") is None:
                    e["disarmed"] = 1
            # (a swinging sign turned by its script counts too: the smallest turn)
            moved = pos is not None and (max(abs(a - b) for a, b in zip(pos[:3], orig["pos"])) > 0.01
                                         or max(abs(a - b) for a, b in zip(pos[3:], orig["rot"])) > 1e-4)
            acdt = esscells.sub(g, "ACDT")
            if acdt or tables["objects"].get(orig["id"], ("",))[0] in ("NPC_", "CREA"):
                if acdt:
                    dyn = struct.unpack_from("<6f", acdt, 40)        # health, fatigue, magicka: (current, base)
                    e["health"], e["fatigue"] = round(dyn[0], 2), round(dyn[2], 2)
                    if struct.unpack_from("<I", acdt, 12)[0] & 4:
                        e["talked"] = 1
                acsc = esscells.sub(g, "ACSC")
                if acsc and acsc[17] & 2:
                    e["dead"] = 1
                if moved:
                    e["pos"], e["yaw"] = [round(v, 2) for v in pos[:3]], round(pos[5], 4)
                ch = npcc.get((base.lower(), n))
                if ch:
                    e["disp"] = ch.get("NPDT")[0]
                    e["contents"] = item_list(ch, tables)
                elif (base.lower(), n) in crec:
                    e["contents"] = item_list(crec[(base.lower(), n)], tables)
            else:
                if moved:
                    e["opos"], e["orot"] = [round(v, 2) for v in pos[:3]], [round(v, 4) for v in pos[3:]]
                if (base.lower(), n) in cntc:
                    e["contents"] = item_list(cntc[(base.lower(), n)], tables)
            if pos:
                e["_at"] = list(pos)          # (where the PC save has it: merge() reads it, write_save drops it)
            refs.append(e)
            k = next((j for j, s in enumerate(g[1:], 1) if s[0] == "SCRI"), None)
            if k is not None:
                sc = script_state(tables, esscells.read_scri(script_subs(g, k)), dropped)
                if sc:
                    scripts.append(dict(sc, ref=i, ref_c=file, ref_id=e["id"]))
    # global scripts: running or stopped, their locals, and the reference one was started on
    for r in by.get("SCPT", []):
        h = r.get("SCHD")
        vals = esscells.read_scri(r.subs) or (None, (), (), ())
        sc = script_state(tables, (text(h[:32]), *vals[1:]), dropped)
        if not sc:
            continue
        rn = r.get("RNAM")
        sc["running"] = int(rn is not None)
        if rn:
            target = struct.unpack_from("<I", rn)[0]
            tf = world.where.get(target & 0xFFFFFF) if target >> 24 == mw else None
            ti = world.link(tf)[0].get(target & 0xFFFFFF) if tf else None
            if ti is not None:
                sc["tgt_c"], sc["tgt_i"] = tf, ti
        scripts.append(sc)
    if missed:
        dropped.append(f"{missed} changed objects not found in the converted cells")
    return refs, scripts, drops


# What a PC save holds of the engine's save, and so is taken from it over the side file's (ess_export.py's
# <name>.mw3ds); everything else comes back from the side file as it was
PC_TOP = ("cell", "feet", "yaw", "hour", "controls", "jumping", "no_teleport", "bounty", "journal_index", "topics",
          "faction_ranks", "faction_rep", "expelled", "mark", "made_spells", "potions", "dead_counts")
PC_STATS = ("name", "race", "class", "custom_class", "birthsign", "female", "head", "hair", "level", "level_progress",
            "skill_progress", "attr_bonus", "skill_bonus", "attr_ups", "bars", "attr_damage", "spells")
# a changed object's fields the PC save always carries, and those it carries only with an actor's or a container's
# own records (ACDT, NPCC / CREC / CNTC)
PC_REF = ("enabled", "taken", "lock", "disarmed", "dead", "opos", "orot", "pos", "yaw")
REF_DEFAULTS = {"enabled": 1, "taken": 0, "lock": 0, "disarmed": 0, "dead": 0, "talked": 0}
PC_REF_OPTIONAL = {"contents": "contents", "disp": "disp", "health": "health", "fatigue": "health",
                   "talked": "health"}


def same(a, b):
    """Equal as both games keep them: numbers to a float's precision (the PC save keeps floats, the 3DS JSON a
    rounding of them), lists and dicts item by item."""
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= max(1e-2, 1e-6 * max(abs(a), abs(b)))
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    return a == b


def keep(base, new):
    """The side file's value where the PC save holds the same (no rounding creeps in), else the PC save's."""
    return base if same(base, new) else new


def side_file(ess, name):
    """The 3DS save an .ess was made from (ess_export.py's <name>.mw3ds): beside it under its own name, else the
    newest one in its folder for the same character (the PC game saved under another name since)."""
    own = ess.with_suffix(".mw3ds")
    found = [own] if own.is_file() else sorted(ess.parent.glob("*.mw3ds"), key=lambda p: -p.stat().st_mtime)
    for p in found:
        try:
            side = esscells.read_json(p)
        except (OSError, ValueError):
            continue
        if side.get("char") == name:
            return side
    return None


def merge(side, pc, engine_globals):
    """The side file's 3DS save with what the PC save holds laid over it."""
    base = json.loads(json.dumps(side["save"]))
    out = dict(base)
    for k in PC_TOP:
        if k in pc:
            # (topics are a set: the order they come back in doesn't count)
            unordered = k == "topics" and sorted(base.get(k) or []) == sorted(pc[k])
            out[k] = base.get(k) if unordered else keep(base.get(k), pc[k])
    # (a spot on an exterior cell's edge names either cell: the 3DS's stays while the player stood still)
    if (out.get("feet") is base.get("feet") and str(base.get("cell", "")).startswith("ext_")
            and str(pc.get("cell", "")).startswith("ext_")):
        out["cell"] = base["cell"]
    st = dict(base["stats"])
    for k in PC_STATS:
        if k in ("race", "class", "birthsign") and str(base["stats"].get(k, "")).lower() == str(pc["stats"].get(k)).lower():
            continue                         # (record ids: the case they are written in doesn't count)
        if k == "class" and base["stats"].get(k) == "$custom" and "custom_class" in pc["stats"]:
            continue                         # (the 3DS's name for a made class: the PC names it NEWCLASSID_CHARGEN)
        if k in pc["stats"]:
            st[k] = keep(base["stats"].get(k), pc["stats"][k])
        elif k == "custom_class":
            st.pop(k, None)
    out["stats"] = st
    # the inventory: the PC save's, with the 3DS's own made things the PC can't name (enchanted by the player)
    named = {i["id"] for i in pc["inventory"]}
    inv = [i for i in base.get("inventory", []) if i["id"] not in named and i["id"] in side.get("unnamed", [])]
    out["inventory"] = keep(base.get("inventory"), pc["inventory"]) + inv if inv else keep(base.get("inventory"),
                                                                                          pc["inventory"])
    # the journal: the 3DS's own when the PC wrote no entry since (same entries), else the PC's in its order
    ent = lambda j: sorted((e["quest"], e["index"]) for e in j)
    out["journal"] = base.get("journal", []) if ent(base.get("journal", [])) == ent(pc["journal"]) else pc["journal"]
    if ent(base.get("journal", [])) != ent(pc["journal"]):
        out["quests_finished"] = pc.get("quests_finished", [])
    g = dict(base.get("globals", {}))
    # (the export wrote the clock from the 3DS's hour count: when that is what came back, the 3DS's own globals stay)
    bh = float(base.get("hour", 0.0))
    clock = {"gamehour": bh % 24.0, "dayspassed": float(int(bh // 24.0) + 1)}
    for k, v in pc.get("globals", {}).items():
        if k in clock and k in g and same(v, clock[k]):
            continue
        if k in g or k in engine_globals:
            g[k] = keep(g.get(k), v)
    out["globals"] = g
    # objects: each one the PC save holds over the 3DS's; one the 3DS sent but the PC no longer lists was put back
    written = {tuple(k) for k in side.get("written", [])}
    refs = {(r["c"], r["i"]): r for r in base.get("refs", [])}
    for key in written:
        if key not in {(e["c"], e["i"]) for e in pc["refs"]}:
            refs.pop(key, None)
    for e in pc["refs"]:
        key = (e["c"], e["i"])
        b = refs.get(key)
        n = dict(b) if b else {"c": e["c"], "i": e["i"], "id": e["id"]}
        at = e.get("_at")
        for f in PC_REF:
            if f in e:
                n[f] = keep(n.get(f), e[f])
            elif f in n and n[f] == REF_DEFAULTS.get(f, object()):
                pass                         # (an older save writes a field at its default: so does this)
            elif f in ("opos", "orot", "pos", "yaw") and not at:
                pass                         # (gone from the world: the PC save keeps no place for it)
            elif f in ("opos", "pos") and at and f in n and same(n[f], at[:3]):
                pass                         # (moved, to where it already stood: the PC save can't tell)
            elif f in ("orot", "yaw") and at and f in n and same(n[f], at[3:] if f == "orot" else at[5]):
                pass
            else:
                n.pop(f, None)
        for f, carrier in PC_REF_OPTIONAL.items():
            if carrier in e:
                if f == "disp" and f in e and isinstance(n.get(f), (int, float))                         and max(0, min(255, int(n[f]))) == e[f]:
                    pass                     # (the PC keeps a disposition in 0-255: one outside it comes back clamped)
                elif f in e:
                    n[f] = keep(n.get(f), e[f])
                elif n.get(f) != REF_DEFAULTS.get(f, object()):
                    n.pop(f, None)
        if b is None and set(n) <= {"c", "i", "id"}:
            continue                         # (written for its script alone: nothing of its own changed)
        refs[key] = n
    out["refs"] = list(refs.values())
    # scripts: their variables from the PC save, the rest (an item's script whose item is gone) as they were
    skey = lambda s: (s["name"].lower(), s.get("ref_c", ""), s.get("ref", -1), s.get("item", ""))
    scripts = {skey(s): s for s in base.get("scripts", [])}
    # (an item picked up keeps on the 3DS the script it ran in the world, under its old place: the PC carries it)
    carried = {(s["name"].lower(), s["item"]): skey(s) for s in base.get("scripts", []) if s.get("item")}
    for s in pc.get("scripts", []):
        if skey(s) not in scripts and s.get("item") and (s["name"].lower(), s["item"]) in carried:
            s = dict(s, **{k: v for k, v in scripts[carried[(s["name"].lower(), s["item"])]].items()
                           if k in ("ref", "ref_c", "ref_id")})
        b = scripts.get(skey(s))
        if b:
            b = dict(b)
            if "locals" in b or any(s["locals"]):
                b["locals"] = keep(b.get("locals"), s["locals"])
            b["running"] = s.get("running", b.get("running", 1))
            for f in ("tgt_c", "tgt_i"):
                if f in s:
                    b[f] = s[f]
            scripts[skey(s)] = b
        else:
            scripts[skey(s)] = s
    out["scripts"] = list(scripts.values())
    # version 1 writes every object in the JSON; the side file's kept cells came in with its refs
    out["version"] = 1
    return out



def write_save(save, path, place, ess):
    for r in save.get("refs", []):
        r.pop("_at", None)
    data = json.dumps(save, separators=(",", ":")).encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + zlib.compress(data, 6))
    # (savegamedialog.cpp readInfo: name, place, level, time in ms since 1900, save count; 0 sorts it by its time)
    made = int(ess.stat().st_mtime * 1000) + EPOCH_1900_MS
    st = save["stats"]
    Path(str(path) + ".meta").write_text(f"{st['name']}\t{place}\t{st['level']}\t{made}\t0\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saves", nargs="+", type=Path, help="Morrowind .ess saves")
    ap.add_argument("--data", type=Path, required=True, help="the converted game (out/world)")
    ap.add_argument("--out", type=Path, required=True, help="folder to write 3ds/mw3ds/saves/ into")
    args = ap.parse_args()

    tables = game_tables(args.data)
    esm = mwfiles.DATA_FILES / "Morrowind.esm"
    tables["world"] = esscells.World(esm, args.data)
    tables["scripts"] = esscells.script_tables(args.data, esm)
    infos = journal_infos(esm)
    failed = 0
    for ess in args.saves:
        print(f"{ess.name}:")
        try:
            save, dropped, place = convert(ess, tables, infos, print)
            side = side_file(ess, save["stats"]["name"])
            if side:
                save = merge(side, save, tables["globals"])
                dropped = [d for d in dropped if not d.startswith("script ")]
                print("  with the 3DS save it came from (.mw3ds): what the PC format can't hold comes back from it")
        except (ValueError, StopIteration, KeyError, struct.error) as e:
            print(f"  not converted: {e or 'a record the save should have is missing'}")
            failed += 1
            continue
        out = args.out / "3ds" / "mw3ds" / "saves" / f"pc_{mwfiles.safe_name(ess.stem)}.json"
        write_save(save, out, place, ess)
        st = save["stats"]
        print(f"  {st['name']}, level {st['level']} {st['race']} {st['class']}, in {place} ({save['cell']}): "
              f"{len(save['inventory'])} items, {len(save['journal'])} journal entries, {len(save['refs'])} objects "
              f"changed, {len(save['scripts'])} scripts -> {out}")
        for d in dropped:
            print(f"  left out (not in Morrowind.esm): {d}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
