"""Converts the engine's saves back into Morrowind's own (.ess), to go on playing them in the PC game.

    python tools/convert/ess_export.py <save.json> [more ...] --data out/world --template <a .ess> --out <folder>

The 3DS save holds the game's state but not everything a PC save carries (the save's screenshot, the world map's
picture, the PC game's own bookkeeping), so one of the player's own PC saves is the template those parts are taken
from; anything about the template's character and world is replaced. Writes <folder>/<save name>.ess and
<save name>.mw3ds beside it: copy both to Morrowind's Saves folder and load the .ess from the PC game's Load menu. The .mw3ds is the whole 3DS save: ess.py takes back from it whatever the PC
format can't hold, so a save sent to the PC and back is the save it was.

What goes across is what ess.py brings the other way: the character, inventory, journal, quests, topics, globals,
factions, the Mark, the world's changed objects (taken, moved, disabled, unlocked, disarmed, the dead, containers'
and people's contents, dispositions) and the scripts' variables (global, objects', carried items'). Not: creatures
the game rolled from a levelled list (they are rolled again), objects the 3DS made (dropped items, placed
creatures), running spell effects, the local map's fog.

Only Morrowind.esm's objects are written; the template's other masters (Tribunal, Bloodmoon) stay listed, so the
PC game loads the save with the same data files as the template's.
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esscells
import mwfiles
from ess import CUSTOM_CLASS, PLAYER_NPC, PLAYER_REF, creation_stats, game_tables, journal_infos
from mwfiles import read_records, text

MONTHS = ["Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed", "Mid Year", "Sun's Height",
          "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"]


def zs(s, size=None):
    b = mwfiles.text_bytes(s)
    if size is None:
        return b + b"\0"
    return (b + b"\0" * size)[:size - 1] + b"\0"


def record(tag, subs, flags=0):
    body = b"".join(t.encode("latin-1") + struct.pack("<I", len(v)) + v for t, v in subs)
    return tag.encode("latin-1") + struct.pack("<III", len(body), 0, flags) + body


def esm_names(esm):
    """Ids as Morrowind.esm spells them, by lower case: topics and quests (DIAL), factions, globals and their type,
    and each quest's name (the journal's title, the INFO with QSTN) with its INFO ids by stage."""
    names, glob_type, quest_title, infos = {}, {}, {}, {}
    quest = None
    for r in read_records(esm, {"DIAL", "INFO", "FACT", "GLOB", "RACE", "CLAS", "BSGN"}):
        if r.tag == "INFO":
            if quest:
                data = r.get("DATA")
                stage = struct.unpack_from("<i", data, 4)[0] if data and len(data) >= 8 else 0
                infos.setdefault(quest.lower(), {})[stage] = r.zstr("INAM")
                if r.get("QSTN") is not None:
                    quest_title[quest.lower()] = r.zstr("NAME")
            continue
        names[r.id.lower()] = r.id
        if r.tag == "DIAL":
            d = r.get("DATA")
            quest = r.id if d and d[0] == 4 else None
        elif r.tag == "GLOB":
            glob_type[r.id.lower()] = r.zstr("FNAM") or "f"
    # Morrowind.esm's quests have no names: Tribunal gave them theirs, and a PC journal files entries by them
    for extra in ("Tribunal.esm", "Bloodmoon.esm"):
        path = Path(esm).with_name(extra)
        if not path.is_file():
            continue
        quest = None
        for r in read_records(path, {"DIAL", "INFO"}):
            if r.tag == "DIAL":
                d = r.get("DATA")
                quest = r.id.lower() if d and d[0] == 4 else None
            elif quest and r.get("QSTN") is not None and quest not in quest_title:
                quest_title[quest] = r.zstr("NAME")
    return names, glob_type, quest_title, infos


class Exporter:
    def __init__(self, data, esm, template):
        self.tables = game_tables(Path(data))
        self.world = esscells.World(esm, data)
        self.scripts = esscells.script_tables(data, esm)
        self.names, self.glob_type, self.quest_title, self.infos = esm_names(esm)
        self.template = list(read_records(template))
        masters = [text(v) for t, v in self.template[0].subs if t == "MAST"]
        self.mw = next((k + 1 for k, m in enumerate(masters) if m.lower() == "morrowind.esm"), 1)
        self.actor_type = {}
        for p in sorted(Path(data).glob("game_actors_*.json")):
            for a in esscells.read_json(p)["actors"]:
                self.actor_type[a["id"].lower()] = "NPC_"
        for k, (t, _) in self.tables["objects"].items():
            if t in ("NPC_", "CREA"):
                self.actor_type[k] = t

    def name(self, id_lower):
        return self.names.get(id_lower.lower(), id_lower)

    def tmpl(self, tag, rid=None):
        return next((r for r in self.template if r.tag == tag and (rid is None or r.id == rid)), None)

    # -- the player

    def player_records(self, save, notes):
        st = save["stats"]
        race = self.tables["races"].get(st["race"].lower())
        custom = st.get("custom_class")
        cls = custom or self.tables["classes"].get(st["class"].lower())
        born_attrs, born_skills = creation_stats(race, cls, st.get("female", 0), self.tables["skills"])
        attrs = [born_attrs[i] + st.get("attr_bonus", [0] * 8)[i] for i in range(8)]
        skills = [born_skills[s] + st.get("skill_bonus", [0] * 27)[s] for s in range(27)]
        bars = st.get("bars", [50, 50, 50, 50, 50, 50])
        out = {}
        if custom:
            cldt = struct.pack("<2i i", *custom["attributes"], custom["specialization"])
            cldt += b"".join(struct.pack("<2i", custom["minor"][k], custom["major"][k]) for k in range(5))
            cldt += struct.pack("<2i", 1, 0)
            out["CLAS"] = record("CLAS", [("NAME", zs(CUSTOM_CLASS)), ("FNAM", zs(custom["name"])), ("CLDT", cldt),
                                          ("DESC", zs(custom.get("desc", "")))])
        t = self.tmpl("NPC_", PLAYER_NPC)
        npdt = bytearray(t.get("NPDT"))
        struct.pack_into("<h", npdt, 0, st.get("level", 1))
        npdt[2:10] = bytes(max(0, min(255, v)) for v in attrs)
        npdt[10:37] = bytes(max(0, min(255, v)) for v in skills)
        struct.pack_into("<3h", npdt, 38, int(bars[1]), int(bars[3]), int(bars[5]))
        struct.pack_into("<i", npdt, 48, 0)
        flag = struct.unpack("<I", t.get("FLAG"))[0] & ~1 | (1 if st.get("female") else 0)
        subs = [("NAME", zs(PLAYER_NPC)), ("FNAM", zs(st["name"])), ("RNAM", zs(race["id"] if race else st["race"])),
                ("CNAM", zs(CUSTOM_CLASS if custom else (cls["id"] if cls else st["class"]))), ("ANAM", zs("")),
                ("BNAM", zs(st.get("head", ""))), ("KNAM", zs(st.get("hair", ""))), ("NPDT", bytes(npdt)),
                ("FLAG", struct.pack("<I", flag))]
        subs += [("NPCS", zs(self.name(s), 32)) for s in st.get("spells", [])]
        out["NPC_"] = record("NPC_", subs)

        # the inventory: stacks, each item's own data and script, then what is worn
        inv = []
        worn = []
        scripts = {s["item"]: s for s in save.get("scripts", []) if s.get("item")}
        k = -1
        for it in save.get("inventory", []):
            if it["id"] not in self.tables["objects"] and it["id"] not in {p["id"] for p in save.get("potions", [])}:
                continue                         # (an item the 3DS made that the PC save can't name: left out)
            k += 1                               # (WIDX counts the stacks written)
            inv.append(("NPCO", struct.pack("<i", it["count"]) + zs(self.name(it["id"]), 32)))
            extra = []
            sc = scripts.get(it["id"])
            if sc and sc["name"].lower() in self.scripts:
                extra += esscells.scri_subs(sc["name"], *esscells.locals_to_mw(self.scripts[sc["name"].lower()],
                                                                               sc.get("locals", [])))
            if it.get("charge", -1) >= 0:
                extra.append(("XCHG", struct.pack("<f", it["charge"])))
            if it.get("soul"):
                extra.append(("XSOL", zs(self.name(it["soul"]))))
            if it.get("condition", -1) >= 0 or extra:
                inv.append(("XIDX", struct.pack("<I", 0)))
                inv += extra
                # (no condition on the 3DS: never worn down, at the item's full health)
                cond = it.get("condition", -1)
                inv.append(("XHLT", struct.pack("<i", cond if cond >= 0 else self.tables["health"].get(it["id"], 0))))
            if it.get("equipped"):
                worn.append(("WIDX", struct.pack("<ii", k, 0)))
        out["NPCC"] = record("NPCC", [("NAME", zs(PLAYER_REF)), ("NPDT", struct.pack("<BBBBi", 50, 0, 0, 0, 0))]
                             + inv + worn)

        # the player's reference: position, health / fatigue / magicka, attributes and skills now
        ref = self.tmpl("REFR")
        feet, yaw = save["feet"], save.get("yaw", 0.0)
        acdt = bytearray(ref.get("ACDT"))
        struct.pack_into("<6f", acdt, 40, bars[0], bars[1], bars[4], bars[5], bars[2], bars[3])
        dmg = st.get("attr_damage", [0] * 8)
        for i in range(8):
            struct.pack_into("<2f", acdt, 80 + 8 * i, attrs[i] - dmg[i], attrs[i])
        sdmg = st.get("skill_damage", [0] * 27)
        chrd = b"".join(struct.pack("<2i", skills[s], int(skills[s] - sdmg[s])) for s in range(27))
        rsubs = []
        for t2, v in ref.subs:
            if t2 == "ACDT":
                v = bytes(acdt)
            elif t2 == "CHRD":
                v = chrd
            elif t2 == "DATA":
                v = struct.pack("<6f", feet[0], feet[1], feet[2], 0.0, 0.0, yaw)
            rsubs.append((t2, v))
        out["REFR"] = record("REFR", rsubs)

        # PCDT: topics, the Mark, flags and progress, bounty, birthsign, last exterior, factions
        pc = self.tmpl("PCDT")
        pnam = bytearray(pc.get("PNAM"))
        flags = struct.unpack_from("<i", pnam)[0]
        for bit, on in ((0x4, not save.get("controls", 1)), (0x1000, not save.get("jumping", 1)),
                        (0x40000, save.get("no_teleport", False))):
            flags = flags | bit if on else flags & ~bit
        struct.pack_into("<iI", pnam, 0, flags, st.get("level_progress", 0))
        struct.pack_into("<27f", pnam, 8, *st.get("skill_progress", [0.0] * 27))
        pnam[116:124] = bytes(max(0, min(255, v)) for v in st.get("attr_ups", [0] * 8))
        psubs = [("DNAM", zs(self.name(t2))) for t2 in save.get("topics", [])]
        mark = save.get("mark")
        if mark:
            c = self.world.cells.get(mark["cell"], {})
            gx, gy = (0, 0) if c.get("interior", True) else c["grid"]
            struct.pack_into("<4f2i", pnam, 144, *mark["pos"], mark.get("yaw", 0.0), gx, gy)
            psubs.append(("MNAM", zs(self.world.display_name(mark["cell"]))))
        psubs.append(("PNAM", bytes(pnam)))
        for t2 in ("SNAM", "NAM9"):
            if pc.get(t2) is not None:
                psubs.append((t2, pc.get(t2)))
        if save.get("bounty"):
            psubs.append(("CNAM", struct.pack("<i", int(save["bounty"]))))
        psubs.append(("BNAM", zs(self.name(st.get("birthsign", "")))))
        lo = save.get("last_outside") or feet
        psubs.append(("ENAM", struct.pack("<2i", int(lo[0] // esscells.CELL_SIZE), int(lo[1] // esscells.CELL_SIZE))))
        expelled = set(save.get("expelled", []))
        ranks, reps = save.get("faction_ranks", {}), save.get("faction_rep", {})
        for fid in list(ranks) + [f for f in reps if f not in ranks]:
            member = fid in ranks
            psubs.append(("FNAM", struct.pack("<B3xiB3x", max(0, ranks.get(fid, 0)), reps.get(fid, 0),
                                              (1 if member else 0) | (2 if fid in expelled else 0))
                          + zs(self.name(fid), 32)))
        if pc.get("KNAM") is not None:
            psubs.append(("KNAM", pc.get("KNAM")))
        out["PCDT"] = record("PCDT", psubs)
        if race and self.tmpl("NPC_", PLAYER_NPC).zstr("RNAM").lower() != race["id"].lower():
            notes.append("the template's running spell effects (its race's and birthsign's abilities among them) left "
                         "out: its character differs. Check the abilities in the PC game's magic menu")
        return out

    # -- the world

    def world_records(self, save, notes):
        """CELL records with the changed references, and the NPCC / CREC / CNTC records their contents need."""
        by_cell = {}
        for r in save.get("refs", []):
            by_cell.setdefault(r["c"], []).append(r)
        scripts = {(s.get("ref_c"), s.get("ref")): s for s in save.get("scripts", [])
                   if s.get("ref", -1) >= 0 and not s.get("item")}
        # an object whose script's variables changed is written for them alone
        have = {(r["c"], r["i"]) for r in save.get("refs", [])}
        for (file, i), s in scripts.items():
            if (file, i) not in have:
                by_cell.setdefault(file, []).append({"c": file, "i": i, "id": s.get("ref_id", "")})
        cells, changes, skipped = [], [], 0
        counter = {}
        for file, refs in by_cell.items():
            c = self.world.cells.get(file)
            if not c:
                skipped += len(refs)
                continue
            _, to_mw = self.world.link(file)
            subs = [("NAME", zs(c["name"])), ("DATA", c["data"])]
            for e in sorted(refs, key=lambda e: e["i"]):
                idx = to_mw.get(e["i"])
                orig = c["refs"].get(idx) if idx is not None else None
                # (a levelled creature's spawn point: the PC game rolls its own creature again)
                if orig is None or orig["id"] in self.world.leveled:
                    skipped += 1
                    continue
                subs += self.ref_subs(e, orig, scripts.get((file, e["i"])), counter, changes)
                self.written.append([file, e["i"]])
            cells.append(record("CELL", subs))
        if skipped:
            notes.append(f"{skipped} changed objects not written (levelled creatures, or not in Morrowind.esm)")
        return cells, changes

    def ref_subs(self, e, orig, script, counter, changes):
        oid = orig["id"]
        kind = self.actor_type.get(oid) or self.tables["objects"].get(oid, ("",))[0]
        name = self.name(oid)
        if e.get("contents") is not None and kind in ("NPC_", "CREA", "CONT"):
            n = counter.get(oid, 0)
            counter[oid] = n + 1
            items = []
            for it in e["contents"]:
                items.append(("NPCO", struct.pack("<i", it[0]) + zs(self.name(it[1]), 32)))
                if len(it) >= 5 and (it[2] >= 0 or it[3] or it[4] >= 0):
                    items.append(("XIDX", struct.pack("<I", 0)))
                    if it[4] >= 0:
                        items.append(("XCHG", struct.pack("<f", it[4])))
                    if it[3]:
                        items.append(("XSOL", zs(self.name(it[3]))))
                    items.append(("XHLT", struct.pack("<i", max(0, it[2]))))
            if kind == "NPC_":
                disp = int(e.get("disp", 50))
                changes.append(record("NPCC", [("NAME", zs(name)), ("NPDT", struct.pack("<BBBBi", max(0, min(255, disp)),
                                                                                       0, 0, 0, n))] + items))
            else:
                changes.append(record("CREC" if kind == "CREA" else "CNTC",
                                      [("NAME", zs(name)), ("INDX", struct.pack("<i", n))] + items))
            name = "%s%08d" % (name, n)
        out = [("FRMR", struct.pack("<I", (self.mw << 24) | orig["frmr"])), ("NAME", zs(name))]
        if e.get("taken"):
            return out + [("DELE", struct.pack("<I", 0))]
        # the reference's own data as Morrowind.esm has it, with its lock and trap as they are now
        for t, v in orig["raw"]:
            if t in ("NAME", "DATA", "DELE", "FRMR", "MVRF", "CNDT"):
                continue
            if t == "FLTV":
                lock = e.get("lock", 0)
                v = struct.pack("<i", lock)
            if t == "TNAM" and e.get("disarmed"):
                continue
            out.append((t, v))
        if e.get("lock", 0) != 0 and orig["lock"] == 0:
            out.append(("FLTV", struct.pack("<i", e["lock"])))
        if kind in ("NPC_", "CREA") and e.get("dead"):
            acdt = bytearray(264)
            struct.pack_into("<I", acdt, 12, 4 if e.get("talked") else 0)
            hp, fat = e.get("health", 0.0), e.get("fatigue", 0.0)
            struct.pack_into("<6f", acdt, 40, hp, hp, fat, fat, 0.0, 0.0)
            acsc = bytearray(112)
            acsc[17] = 2
            out += [("ACDT", bytes(acdt)), ("ACSC", bytes(acsc))]
        if script and script["name"].lower() in self.scripts:
            out += esscells.scri_subs(script["name"], *esscells.locals_to_mw(self.scripts[script["name"].lower()],
                                                                           script.get("locals", [])))
        if not e.get("enabled", 1):
            out.append(("ZNAM", b"\0"))
        pos, rot = list(orig["pos"]), list(orig["rot"])
        if "opos" in e:
            pos, rot = e["opos"], e.get("orot", rot)
        elif "pos" in e:
            pos, rot = e["pos"], [rot[0], rot[1], e.get("yaw", rot[2])]
        out.append(("DATA", struct.pack("<6f", *pos, *rot)))
        return out

    # -- globals, quests, the journal

    def global_records(self, save):
        ours = {k.lower(): v for k, v in save.get("globals", {}).items()}
        # the clock from the engine's own count of hours since the first day began (its DaysPassed global ran a day
        # short in saves made before that was fixed): the hour of the day, and the days, from 1
        hour = float(save.get("hour", ours.get("gamehour", 9.0)))
        ours["gamehour"], ours["dayspassed"] = hour % 24.0, float(int(hour // 24.0) + 1)
        out, seen = [], set()
        for r in self.template:
            if r.tag != "GLOB":
                continue
            gid = r.id.lower()
            seen.add(gid)
            v = ours.get(gid, struct.unpack("<f", r.get("FLTV"))[0])
            out.append(record("GLOB", [("NAME", zs(r.id)), ("FNAM", r.get("FNAM")), ("FLTV", struct.pack("<f", v))]))
        for gid, v in ours.items():
            if gid not in seen and gid in self.glob_type:
                out.append(record("GLOB", [("NAME", zs(self.name(gid))), ("FNAM", self.glob_type[gid].encode()),
                                           ("FLTV", struct.pack("<f", v))]))
        return out

    def script_records(self, save):
        out = []
        for s in save.get("scripts", []):
            if s.get("item") or s.get("ref", -1) >= 0:
                continue
            sc = self.scripts.get(s["name"].lower())
            if not sc or "schd" not in sc:
                continue
            subs = [("SCHD", zs(sc["name"], 32) + sc["schd"])]
            subs += esscells.scri_subs(None, *esscells.locals_to_mw(sc, s.get("locals", [])))
            if s.get("running", 1):
                target = 0xFFFFFFFF
                if s.get("tgt_c") is not None:
                    _, to_mw = self.world.link(s["tgt_c"])
                    idx = to_mw.get(s.get("tgt_i", -1))
                    if idx is not None:
                        target = (self.mw << 24) | idx
                subs.append(("RNAM", struct.pack("<I", target)))
            out.append(record("SCPT", subs))
        return out

    def journal_records(self, save, notes):
        dial = [record("DIAL", [("NAME", zs(self.name(q))), ("XIDX", struct.pack("<i", i))])
                for q, i in save.get("journal_index", {}).items()]
        ques, html = {}, []
        day, month = int(save.get("globals", {}).get("day", 1)), int(save.get("globals", {}).get("month", 7))
        for j in save.get("journal", []):
            info = self.infos.get(j["quest"], {}).get(j["index"])
            title = (self.quest_title.get(j["quest"]) or self.tables["journal"].get(j["quest"], {}).get("title")
                     or self.name(j["quest"]))
            if info and title:
                ques.setdefault(title, []).append(info)
            html.append('<FONT COLOR="9F0000">%d %s</FONT><BR>\r\n%s<P>\r\n' % (day, MONTHS[month % 12], j["text"]))
        recs = dial + [record("QUES", [("NAME", zs(t))] + [("DATA", zs(i)) for i in ids]) for t, ids in ques.items()]
        recs.append(record("JOUR", [("NAME", mwfiles.text_bytes("".join(html)))]))
        if save.get("journal"):
            notes.append("the journal's entries are all dated the save's day (the 3DS doesn't keep each one's date)")
        return recs

    def made_records(self, save):
        """Spells the player made and potions brewed: SPEL and ALCH records of their own, as the PC game saves them."""
        out = []
        for s in save.get("made_spells", []):
            enams = [("ENAM", struct.pack("<hbbiiiii", e[0], e[1], e[2], e[6], 0, e[5], e[3], e[4]))
                     for e in s["effects"]]
            out.append(record("SPEL", [("NAME", zs(s["id"])), ("FNAM", zs(s["name"])),
                                       ("SPDT", struct.pack("<3i", 0, int(s.get("cost", 0)), 0))] + enams))
        for p in save.get("potions", []):
            enams = [("ENAM", struct.pack("<hbbiiiii", e[0], e[1], e[2], 0, 0, e[4], e[3], e[3])) for e in p["effects"]]
            out.append(record("ALCH", [("NAME", zs(p["id"])), ("MODL", zs("m\\Misc_Potion_Bargain_01.nif")),
                                       ("FNAM", zs(p["name"])),
                                       ("ALDT", struct.pack("<fii", p.get("weight", 0.5), int(p.get("value", 0)), 0))]
                       + enams + [("TEXT", zs(p.get("icon", "")))]))
        return out

    def kill_records(self, save):
        subs = []
        for k, n in save.get("dead_counts", {}).items():
            subs += [("KNAM", zs(self.name(k))), ("CNAM", struct.pack("<i", n))]
        return [record("KLST", subs + [("INTV", struct.pack("<i", 0))])] if subs else []

    # -- the file

    def export(self, src):
        save = esscells.read_engine_save(src)
        notes = []
        st = save["stats"]
        player = self.player_records(save, notes)
        self.written = []
        cells, changes = self.world_records(save, notes)
        place = self.world.display_name(save["cell"]) or save["cell"]
        bars = st.get("bars", [50, 50, 50, 50, 50, 50])
        body = []
        body += self.global_records(save)
        body += [player["CLAS"]] if "CLAS" in player else []
        body += self.made_records(save)
        body += self.script_records(save)
        body += [record(r.tag, r.subs, r.flags) for r in self.template if r.tag == "REGN"]
        body.append(player["NPC_"])
        body.append(player["NPCC"])
        body += changes
        body += cells
        body.append(player["REFR"])
        body += self.journal_records(save, notes)
        fmap = self.tmpl("FMAP")
        if fmap:
            body.append(record("FMAP", fmap.subs))
        body.append(player["PCDT"])
        game = self.tmpl("GAME")
        gmdt = bytearray(game.get("GMDT"))
        gmdt[:64] = zs(place, 64)
        body.append(record("GAME", [("GMDT", bytes(gmdt))]))
        tmpl_race = self.tmpl("NPC_", PLAYER_NPC).zstr("RNAM").lower()
        if tmpl_race == st["race"].lower() and self.tmpl("PCDT").zstr("BNAM").lower() == st.get("birthsign", "").lower():
            splm = self.tmpl("SPLM")
            if splm:
                body.append(record("SPLM", splm.subs))
        body += self.kill_records(save)

        head = self.template[0]
        hsubs = []
        for t, v in head.subs:
            if t == "HEDR":
                v = bytearray(v)
                v[40:296] = zs(f"{st['name']}, {place}", 256)
                struct.pack_into("<I", v, 296, len(body))
                v = bytes(v)
            elif t == "GMDT":
                v = bytearray(v)
                struct.pack_into("<3f", v, 0, bars[0], bars[1], save.get("hour", 9.0))
                v[24:88] = zs(place, 64)
                v[92:124] = zs(st["name"], 32)
                v = bytes(v)
            hsubs.append((t, v))
        # The whole 3DS save beside the PC one (<name>.mw3ds): what the PC format has no place for comes back from
        # it (ess.py), and the objects written here are listed, so one the PC game put back as it was is known
        unnamed = [it["id"] for it in save.get("inventory", []) if it["id"] not in self.tables["objects"]
                   and it["id"] not in {p["id"] for p in save.get("potions", [])}]
        side = json.dumps({"char": st["name"], "save": save, "written": self.written, "unnamed": unnamed},
                          separators=(",", ":")).encode("utf-8")
        side = b"MWZ1" + struct.pack("<I", len(side)) + zlib.compress(side, 6)
        return record("TES3", hsubs) + b"".join(body), side, notes, st, place


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("saves", nargs="+", type=Path, help="engine saves (save.json, save_NNN.json)")
    ap.add_argument("--data", type=Path, required=True, help="the converted game the saves were made with (out/world)")
    ap.add_argument("--template", type=Path, required=True, help="one of your own PC saves (.ess)")
    ap.add_argument("--out", type=Path, required=True, help="folder to write the .ess files into")
    args = ap.parse_args()
    ex = Exporter(args.data, mwfiles.DATA_FILES / "Morrowind.esm", args.template)
    args.out.mkdir(parents=True, exist_ok=True)
    for src in args.saves:
        data, side, notes, st, place = ex.export(src)
        dest = args.out / (Path(src).stem.replace(".json", "") + ".ess")
        dest.write_bytes(data)
        dest.with_suffix(".mw3ds").write_bytes(side)
        print(f"{src.name}: {st['name']}, level {st['level']}, in {place} -> {dest} ({len(data) // 1024} KB)")
        for n in notes:
            print("  " + n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
