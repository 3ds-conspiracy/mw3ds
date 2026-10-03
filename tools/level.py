"""Converts the level: Seyda Neen and Balmora with every interior, and all the land between
them. The game starts with character creation in the Census and Excise Office.

The exterior is written one grid cell (8192 units) at a time, cells/ext_<x>_<y>.*; the 3DS keeps
the 3 x 3 cells around the player loaded. The walkable land is the rectangle around the towns'
cells (plus --margin); one more ring is coarse terrain and water only.

  --towns Balmora             other towns (no character creation unless Seyda Neen is included;
                              the game then starts in front of a door of the first town)
  --interiors office / none   fewer interiors (faster conversion, less voice audio)

Writes into out/data/:
  cells/<cell>.cel        geometry, collision, actors, swing doors, sky (convert_cell.py);
                          interiors by name, exterior grid cells as ext_<x>_<y> (m = minus)
  cells/<cell>.act        animated NPCs
  cells/<cell>.json       reference table: ids, positions, bounds, hideable ranges, doors
  textures/*.t3x          sized for the 3DS screen (textures.py)
  game.json               objects, scripts, dialogue, journal, classes, races, ... (build_game.py)
  sound/*.snd, music/*.snd  22 kHz mono PCM16 ('SND1', u32 rate, u32 samples, samples)
"""
import argparse
import audioop
import zlib
import json
import math
import re
import struct
import time
from pathlib import Path

import miniaudio
import numpy as np

import build_game
from actors import write_skeletons
from animlayers import npc_skeletons
from convert_cell import EXTERIOR_SUN_DIR, Converter
from distant import build_distant
from creature import resolve_leveled
from firstperson import FirstPerson
from mwfiles import DATA_FILES, Archives, cell_refs, load_db, read_records
from terrain import CELL_SIZE
from terrain import Terrain
from worldmap import build_map, town_labels
from uiassets import build_ui
from artassets import build_art
from movies import build_movies
from textures import OUT, TextureSet, migrate_flat, shard_dir

START_CELL = "Imperial Prison Ship"          # a new game wakes up here (script CharGen: PositionCell)
START_SPAWN = ((61.0, -135.0, 24.0), 340.0)  # CharGen's PositionCell 61, -135, 24, 340 (degrees)
OFFICE_CELL = "Seyda Neen, Census and Excise Office"
TOWN = "Seyda Neen"                       # the town with character creation
TOWNS = ["Seyda Neen", "Balmora"]         # default level: both towns and the land between them
EXTERIOR = (-2, -9)                       # where the office doors lead
ENTRY_DOOR = "chargen customs door"       # exterior door the player arrives through
END_DOORS = set()                         # doors that end the level (none: the town is open)
# Main quest, up to Caius Cosades' first orders: the dungeons they send the player to (interiors
# whose names start with these) and exterior cells the walkable land must reach (their entrances)
QUEST_INTERIORS = ["Arkngthand", "Andrano Ancestral Tomb"]
QUEST_CELLS = [(0, -2)]                   # Arkngthand's doors, east of Balmora
# Exploring and fighting (Morrowind's Music/Explore and Music/Battle; 22 kHz mono like the rest)
MUSIC = [f"Music/Explore/mx_explore_{k}.mp3" for k in range(1, 8)]
MUSIC_BATTLE = [f"Music/Battle/MW battle{k}.mp3" for k in (1, 2, 3, 5)] + [f"Music/Battle/MW battle {k}.mp3" for k in (4, 7, 8)]
RATE = 22050
VOICE_RATE = 16000          # speech has next to nothing above 8 kHz; the 3DS speakers roll off there too
# game.json sections written to game_<name>.json (source/game.cpp reads them one at a time)
SPLIT_SECTIONS = ["actors", "objects", "scripts", "dialogue", "journal", "gmst", "classes", "races",
                  "birthsigns", "spells", "voices", "ref_cells", "ui", "firstperson", "factions"]
ROOT = Path(__file__).resolve().parents[1]
PART_BYTES = 400 * 1024                   # game_<section>_<k>.json parts of big sections
LINEAR_BUDGET_MB = 24                     # of the 32 MB linear heap, leaving room for frame buffers and audio

# Morrowind.ini [Weather Clear] defaults, used when the ini is missing a value
WEATHER_DEFAULTS = {"Sky Day Color": "095,135,203", "Fog Day Color": "206,227,255",
                    "Ambient Day Color": "137,140,160", "Sun Day Color": "255,252,238",
                    "Cloud Texture": "Tx_Sky_Clear.tga"}


# What OpenMW's Weather class reads besides colours, with the values its built-in fallbacks (files/openmw.cfg) give
# when Morrowind.ini has none: the key written to game.json and the ini key
WEATHER_KEYS = [("delta", "Transition Delta"), ("wind", "Wind Speed"), ("glare", "Glare View"),
                ("rain_threshold", "Rain Threshold"), ("thunder_freq", "Thunder Frequency"),
                ("thunder_threshold", "Thunder Threshold"), ("flash_decrement", "Flash Decrement"),
                ("clouds_max", "Clouds Maximum Percent"), ("precip", "Using Precip")]
WEATHER_VALUES = {"Clear": (.015, .1, 1, 0, 0, 0, 0, 1, 0), "Cloudy": (.015, .2, 1, 0, 0, 0, 0, 1, 0),
                  "Foggy": (.015, 0, .25, 0, 0, 0, 0, 1, 0), "Overcast": (.015, .2, 0, 0, 0, 0, 0, 1, 0),
                  "Rain": (.015, .3, 0, .6, 0, 0, 0, .66, 1), "Thunderstorm": (.03, .5, 0, .6, .4, .6, 4, .66, 1),
                  "Ashstorm": (.035, .8, 0, 0, 0, 0, 0, 1, 0), "Blight": (.04, .9, 0, 0, 0, 0, 0, 1, 0),
                  "Snow": (.015, 0, 0, 0, 0, 0, 0, 1, 0), "Blizzard": (.03, .9, 0, 0, 0, 0, 0, 1, 0)}


# The weathers as regions list their chances (Morrowind.ini [Weather <name>] sections)
WEATHER_NAMES = ["Clear", "Cloudy", "Foggy", "Overcast", "Rain", "Thunderstorm", "Ashstorm", "Blight", "Snow", "Blizzard"]


def snd_name(path):
    stem = re.sub(r"\.[^.]+$", "", path.lower().replace("/", "\\"))
    return re.sub(r"[^a-z0-9_]", "_", stem) + ".snd"


def convert_sound(arch, path, out_dir, rate=RATE, shard=False):
    """path relative to Data Files (e.g. 'Sound\\Fx\\x.wav'); tries .mp3 when the .wav is missing."""
    cands = [path, re.sub(r"\.wav$", ".mp3", path, flags=re.I)]
    src = next((c for c in cands if arch.exists(c)), None)
    if src is None:
        return None
    out = out_dir / (shard_dir(snd_name(src)) if shard else "") / snd_name(src)
    out.parent.mkdir(parents=True, exist_ok=True)
    head = out.open("rb").read(12) if out.exists() else b""
    if head[:4] != b"SND2" or struct.unpack_from("<I", head, 4)[0] != rate:
        dec = miniaudio.decode(arch.read(src), output_format=miniaudio.SampleFormat.SIGNED16,
                               nchannels=1, sample_rate=rate)
        samples = np.frombuffer(dec.samples, dtype="<i2")
        # 'SND2': IMA ADPCM, 4 bits per sample (first sample in the high nibble), a quarter of PCM16
        adpcm, _ = audioop.lin2adpcm(samples.tobytes(), 2, None)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "wb") as f:
            f.write(b"SND2" + struct.pack("<II", rate, len(samples)) + adpcm)
    return out.name


def ref_owners(cell, leveled=()):
    """Ownership of a CELL record's references, in cell_refs order (same filter): per reference a dict
    with "owner" (NPC id), "ofac" / "orank" (faction and least rank), "oglob" (a global that, when set,
    lets the player use it: rented beds)."""
    refs, cur = [], None
    for tag, d in cell.subs:
        if tag == "FRMR":
            cur = {"pos": False, "deleted": False, "own": {}}
            refs.append(cur)
        elif cur is None:
            continue
        elif tag == "DATA":
            cur["pos"] = True
        elif tag == "DELE":
            cur["deleted"] = True
        elif tag in ("ANAM", "BNAM", "CNAM"):
            key = {"ANAM": "owner", "BNAM": "oglob", "CNAM": "ofac"}[tag]
            cur["own"][key] = d.split(bytes(1), 1)[0].decode("latin-1").lower()
        elif tag == "INDX":
            cur["own"]["orank"] = struct.unpack("<i", d)[0]
        elif tag == "TNAM":
            cur["own"]["trap"] = d.split(bytes(1), 1)[0].decode("latin-1").lower()     # trap spell
        elif tag == "NAME":
            cur["name"] = d.split(bytes(1), 1)[0].decode("latin-1").lower()
    return [dict(r["own"], **({"lev": r["name"]} if leveled and r.get("name") in leveled else {}))
            for r in refs if r["pos"] and not r["deleted"]]


def compress_file(path):
    """Lossless: 'MWZ1', u32 original size, zlib stream (source/zfile.cpp inflates on load)."""
    data = path.read_bytes()
    if data[:4] == b"MWZ1":
        return
    packed = zlib.compress(data, 9)
    if len(packed) + 8 < len(data):
        path.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + packed)


def finite(x):
    """Some records hold garbage float bits (NaN / inf); JSON has no such values."""
    if isinstance(x, float):
        return x if x == x and abs(x) != float("inf") else 0.0
    if isinstance(x, dict):
        return {k: finite(v) for k, v in x.items()}
    if isinstance(x, list):
        return [finite(v) for v in x]
    return x


def read_ini():
    path = DATA_FILES.parent / "Morrowind.ini"
    return path.read_text(encoding="cp1252", errors="replace") if path.exists() else ""


def ini_section(ini, name):
    m = re.search(r"^\[%s\]\s*$(.*?)(?=^\[|\Z)" % re.escape(name), ini, re.M | re.S)
    if not m:
        return {}
    return {k.strip(): v.strip() for k, v in (line.split("=", 1) for line in m.group(1).splitlines() if "=" in line)}


def ini_float(kv, key, default):
    """A number from an ini section; a trailing ';' comment and a missing value are tolerated."""
    try:
        return float(kv.get(key, "").split(";")[0].strip())
    except ValueError:
        return default


def class_quiz(ini):
    """The class questions live in Morrowind.ini ([Question 1] .. [Question 10])."""
    quiz = []
    for n in range(1, 11):
        kv = ini_section(ini, f"Question {n}")
        if not kv:
            continue
        quiz.append({"question": kv.get("Question", ""),
                     "answers": [kv.get(k, "") for k in ("AnswerOne", "AnswerTwo", "AnswerThree")],
                     "sound": kv.get("Sound", "")})
    return quiz


def weather(ini, name):
    kv = dict(WEATHER_DEFAULTS, **ini_section(ini, f"Weather {name}"))
    rgb = lambda key: np.array([int(c) for c in kv[key].split(",")[:3]], dtype=float) / 255.0
    return {"sky": rgb("Sky Day Color"), "fog": rgb("Fog Day Color"), "ambient": rgb("Ambient Day Color"),
            "sun": rgb("Sun Day Color"), "clouds": kv["Cloud Texture"]}


def leveled_creatures(db, lid, depth=0):
    """Every creature a leveled creature list can give (following nested lists)."""
    out = set()
    for _, cid in db["LEVC"].get(lid, []) if depth < 5 else []:
        cid = cid.lower()
        if cid in db["LEVC"]:
            out |= leveled_creatures(db, cid, depth + 1)
        elif cid in db["objects"]:
            out.add(cid)
    return out


def spawn_ids(db):
    """Ids scripts and dialogue place with PlaceAtPC / PlaceAtMe (quoted or bare)."""
    texts = list(db["SCPT"].values()) + [i.zstr("BNAM") or "" for d in db["dialogue"] for i in d["infos"]]
    out = set()
    for t in texts:
        for m in re.finditer(r'place(?:atpc|atme)[\s,]+(?:"([^"]+)"|([^\s,"]+))', t, re.I):
            out.add((m.group(1) or m.group(2)).strip().lower())
    return out


def town_cells(db, town):
    """Exterior grid cells named `town`."""
    return [xy for xy, rec in db["CELL"].items() if isinstance(xy, tuple) and (rec.id or "").lower() == town.lower()]


def exterior_name(db, xy):
    """What GetPCCell and door labels call an exterior cell: its own name, else its region's."""
    rec = db["CELL"].get(xy)
    if rec is not None and rec.id:
        return rec.id
    region = rec.zstr("RGNN") if rec is not None else None
    return db["REGN"].get((region or "").lower(), "Wilderness")


SEA_DEPTH = 100.0      # ground this far under the water counts as open water


def sea_coverage(terrain, gx, gy):
    """Share of an exterior cell's ground more than SEA_DEPTH under the (single, z = 0) water level:
    a coast is a third or more, a river a narrow strip of a few percent (the shore sound uses it)."""
    tx, ty = gx - terrain.x0, gy - terrain.y0
    if not terrain.has_land[ty, tx]:
        return 1.0
    h = terrain.h[ty * 64:ty * 64 + 65, tx * 64:tx * 64 + 65]
    return round(float((h < -SEA_DEPTH).mean()), 3)


def exterior_file(xy):
    return "ext_%s%d_%s%d" % ("m" if xy[0] < 0 else "", abs(xy[0]), "m" if xy[1] < 0 else "", abs(xy[1]))


def street_spawn(db, town, core):
    """Where a level without character creation starts: in front of the town door (an interior's
    exit into the streets) nearest the middle of the walkable area. (feet position, yaw) or None."""
    lo = (core[0] * CELL_SIZE, core[1] * CELL_SIZE)
    hi = ((core[2] + 1) * CELL_SIZE, (core[3] + 1) * CELL_SIZE)
    mid = ((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2)
    doors = []
    for name, cell in db["CELL"].items():
        if not (isinstance(name, str) and name.lower().startswith(town.lower() + ",")):
            continue
        for r in cell_refs(cell):
            d = r.get("dest")
            if d is None or r.get("dest_cell") or not (lo[0] <= d[0] < hi[0] and lo[1] <= d[1] < hi[1]):
                continue
            doors.append(d)
    if not doors:
        return None
    # Street level: leave out doors high above the lowest ones (towers, upper floors, rooftops)
    low = min(d[2] for d in doors)
    doors = [d for d in doors if d[2] <= low + 400]
    d = min(doors, key=lambda d: (d[0] - mid[0]) ** 2 + (d[1] - mid[1]) ** 2)
    return d[:3], d[5]


def door_dest(db, cell_xy, door_id, into_cell):
    ext = db["CELL"][cell_xy]
    for r in cell_refs(ext):
        if r["id"].lower() == door_id and r.get("dest_cell") == into_cell:
            return r["dest"]
    raise SystemExit(f"door {door_id} not found in exterior {cell_xy}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-tex", type=int, default=256, help="largest texture edge (texels)")
    ap.add_argument("--towns", default=",".join(TOWNS),
                    help="comma-separated towns; the walkable land is the rectangle around them all")
    ap.add_argument("--interiors", choices=["all", "office", "none"], default="all",
                    help="every interior of the towns, only the Census and Excise Office, or none")
    ap.add_argument("--margin", type=int, default=1, help="walkable cells beyond the towns' rectangle")
    ap.add_argument("--weather", default="Clear", help="Morrowind.ini weather section for the exterior")
    ap.add_argument("--force", action="store_true", help="re-encode textures and sounds")
    ap.add_argument("--world", action="store_true",
                    help="the whole island: every interior, all land walkable (with MW3DS_OUT=out/world to try it)")
    args = ap.parse_args()

    t0 = time.time()
    db = load_db()
    arch = Archives()
    ini = read_ini()
    print(f"ESM loaded in {time.time() - t0:.1f}s")

    textures = TextureSet(arch, args.max_tex, args.force)
    unshard_cells()              # the converter (and its cache) works on cells/<file>.*
    conv = Converter(db, arch, textures)
    towns = [t.strip() for t in args.towns.split(",") if t.strip()]
    chargen = TOWN.lower() in (t.lower() for t in towns) and args.interiors != "none"

    # Walkable land: the rectangle around every town (plus a margin); one more ring of cells around
    # it is converted as coarse terrain and water only, so the view never ends at a hard edge
    named = [xy for t in towns for xy in town_cells(db, t)] or [EXTERIOR]
    for t in towns:
        if not town_cells(db, t):
            print(f"  warning: no exterior cells are named {t!r}")
    x0, x1 = min(x for x, _ in named) - args.margin, max(x for x, _ in named) + args.margin
    y0, y1 = min(y for _, y in named) - args.margin, max(y for _, y in named) + args.margin
    if chargen:
        x0, x1 = min([x0] + [x for x, _ in QUEST_CELLS]), max([x1] + [x for x, _ in QUEST_CELLS])
        y0, y1 = min([y0] + [y for _, y in QUEST_CELLS]), max([y1] + [y for _, y in QUEST_CELLS])
    if args.world:
        land = [xy for xy in db["CELL"] if isinstance(xy, tuple)]
        x0, x1 = min(x for x, _ in land), max(x for x, _ in land)
        y0, y1 = min(y for _, y in land), max(y for _, y in land)
    walk = (x0 * CELL_SIZE, y0 * CELL_SIZE, (x1 + 1) * CELL_SIZE, (y1 + 1) * CELL_SIZE)
    terrain = Terrain(db, x0 - 1, y0 - 1, x1 + 1, y1 + 1)
    print(f"land: walkable cells {x0}..{x1} x {y0}..{y1} ({(x1 - x0 + 1) * (y1 - y0 + 1)} cells) "
          f"+ a horizon ring", flush=True)

    interiors = [START_CELL, OFFICE_CELL] if chargen else []
    if args.world:
        interiors += sorted(n for n in db["CELL"] if isinstance(n, str) and n not in (START_CELL, OFFICE_CELL))
    elif args.interiors == "all":
        for t in towns:
            interiors += sorted(n for n in db["CELL"] if isinstance(n, str) and n.lower().startswith(t.lower() + ",")
                                and n not in (START_CELL, OFFICE_CELL))
        if chargen:
            interiors += sorted(n for n in db["CELL"] if isinstance(n, str)
                                and any(n.lower().startswith(q.lower()) for q in QUEST_INTERIORS))

    # Where a game without character creation starts: in front of a door in the first town
    spawn = None if chargen else street_spawn(db, towns[0], (x0, y0, x1, y1))
    start_xy = (int(spawn[0][0] // CELL_SIZE), int(spawn[0][1] // CELL_SIZE)) if spawn else \
        (named[0] if not chargen else None)

    cells = []
    print(f"converting {len(interiors)} interiors + {(x1 - x0 + 3) * (y1 - y0 + 3)} exterior cells", flush=True)
    failed = []                            # cells that didn't convert: left out, reported at the end
    for name in interiors:
        cspawn = None
        if name == START_CELL:
            cspawn = (START_SPAWN[0], math.radians(START_SPAWN[1]))
        elif name == OFFICE_CELL:
            entry = door_dest(db, EXTERIOR, ENTRY_DOOR, OFFICE_CELL)
            cspawn = (entry[:3], entry[5])
        try:
            c = conv.interior(name, spawn=cspawn)
        except Exception as e:
            print(f"  FAILED {name}: {type(e).__name__}: {e}", flush=True)
            failed.append((name, f"{type(e).__name__}: {e}"))
            continue
        # CELL DATA flag 4: sleeping here is illegal (only waiting, or a bed)
        nosleep = bool(struct.unpack("<I", db["CELL"][name].get("DATA")[:4])[0] & 4)
        cells.append(dict(c, name=name, interior=True, nosleep=nosleep))
    grid = {}
    for gy in range(y0 - 1, y1 + 2):
        for gx in range(x0 - 1, x1 + 2):
            walkable = x0 <= gx <= x1 and y0 <= gy <= y1
            name = exterior_name(db, (gx, gy))
            try:
                c = conv.exterior_cell(gx, gy, terrain, walk, weather(ini, args.weather), walkable, name,
                                       exterior_file((gx, gy)), spawn if (gx, gy) == start_xy else None)
            except Exception as e:
                print(f"  FAILED exterior {gx},{gy}: {type(e).__name__}: {e}", flush=True)
                failed.append((f"exterior {gx},{gy}", f"{type(e).__name__}: {e}"))
                continue
            c = dict(c, name=name, interior=False, grid=[gx, gy], sea=sea_coverage(terrain, gx, gy))
            grid[(gx, gy)] = c
            if (gx, gy) == start_xy:
                cells.insert(0, c)                 # the level starts here
            else:
                cells.append(c)

    # Door routing: exterior destinations (no cell name) land in the grid cell under them
    names = {c["name"] for c in cells if c["interior"]}
    # Actors: one per NPC / creature record, however many times it is placed (creatures are placed
    # hundreds of times); cells' references point at the record's entry
    actor_index, actor_records, actor_cells = {}, [], []
    for c in cells:
        c["actor_map"] = []
        for r in c["npcs"]:
            k = r.id.lower()
            if k not in actor_index:
                actor_index[k] = len(actor_records)
                actor_records.append(r)
                actor_cells.append(set())
            actor_cells[actor_index[k]].add(c["name"])
            c["actor_map"].append(actor_index[k])
    # Creatures and NPCs scripts place at run time (PlaceAtPC / PlaceAtMe): an actor entry each, and their
    # mesh library (the 3DS builds them where the script says)
    spawns = {}
    # ... and the creatures summoning spells call up
    summons = {k for k, r in db["objects"].items() if r.tag == "CREA" and k.endswith("_summon")}
    sleepers = set()
    for lid in db["LEVC"]:
        sleepers |= leveled_creatures(db, lid)
    # ... and the creatures the soul gem tests (tools/tests/openmw-spec-world.txt) place by hand
    test_spawns = {"ancestor_guardian_fgdd", "dagoth daynil", "netch_bull_ranched", "kwama queen_abaesen",
                  "centurion_spider_tga1", "netch_betty_ilgn", "guar"}
    for sid in sorted(spawn_ids(db) | summons | sleepers | test_spawns):
        rec = db["objects"].get(sid)
        if rec is None and sid in db["LEVC"]:
            rec = resolve_leveled(db, sid)
        if rec is None or rec.tag not in ("NPC_", "CREA"):
            continue
        made = conv.spawn_library(rec)
        if not made:
            continue
        k = rec.id.lower()
        if k not in actor_index:
            actor_index[k] = len(actor_records)
            actor_records.append(rec)
            actor_cells.append(set())
        spawns[sid] = {"actor": actor_index[k], "lib": made[0], "skeleton": made[1], "type": rec.tag}
    print(f"placeable at run time: {len(spawns)} creatures / NPCs", flush=True)
    owned_refs, owner_mismatch = 0, []
    for c in cells:
        for ref in c["refs"]:
            d = ref.get("dest")
            if d is not None:
                if d["cell"] is None:
                    xy = (int(d["pos"][0] // CELL_SIZE), int(d["pos"][1] // CELL_SIZE))
                    if xy in grid and x0 <= xy[0] <= x1 and y0 <= xy[1] <= y1:
                        d["cell"], d["grid"] = grid[xy]["name"], list(xy)
                    else:
                        d["cell"], d["unconverted"] = exterior_name(db, xy), True
                elif d["cell"] not in names:
                    d["unconverted"] = True
                if ref["id"].lower() in END_DOORS:
                    d["end"] = True
            if "actor" in ref:
                ref["actor"] = c["actor_map"][ref["actor"]]     # the record's entry in game.json "actors"
        # Path grid in world coordinates (exterior points are stored relative to the cell's corner)
        pg = db["PGRD"].get(tuple(c["grid"]) if not c["interior"] else c["name"])
        path = None
        if pg and pg[0]:
            ox, oy = (c["grid"][0] * CELL_SIZE, c["grid"][1] * CELL_SIZE) if not c["interior"] else (0, 0)
            path = {"points": [[x + ox, y + oy, z] for x, y, z in pg[0]], "edges": [list(e) for e in pg[1]]}
        # Owners (theft, rented beds): added here, the cell cache doesn't hold them
        rec = db["CELL"].get(c["name"] if c["interior"] else tuple(c["grid"]))
        owners = ref_owners(rec, db["LEVC"]) if rec is not None else []
        if len(owners) == len(c["refs"]):
            for ref, own in zip(c["refs"], owners):
                for k in ("owner", "ofac", "orank", "oglob", "trap", "lev"):
                    ref.pop(k, None)
                ref.update(own)
                owned_refs += bool(own)
        elif c["refs"] and owners:
            owner_mismatch.append(c["name"])
        with open(OUT / "cells" / (c["file"] + ".json"), "w") as f:
            json.dump(dict({"name": c["name"], "refs": c["refs"]}, **({"pathgrid": path} if path else {})), f,
                      separators=(",", ":"))

    print(f"owned objects: {owned_refs}" + (f"; owners not matched in {len(owner_mismatch)} cells: "
          f"{owner_mismatch[:5]}" if owner_mismatch else ""), flush=True)
    # Cell files a previous level left behind
    keep = {c["file"] for c in cells} | {"actors"} | {f"skel_{k}" for k in range(len(conv.skeletons))}
    for p in (OUT / "cells").glob("*"):
        if p.stem not in keep:
            p.unlink()
    # Skeletons and their animations, each once for every cell's actors (skel_<k>.skl): the 3DS reads
    # only those its actors use
    # NPC skeletons: base_anim under base_anim_female's few groups, swimming and turning (tools/animlayers.py)
    print("  npc skeletons:", ", ".join(npc_skeletons(conv)))
    for k, sk in enumerate(conv.skeletons):
        write_skeletons(OUT / "cells" / f"skel_{k}.skl", [sk])
    conv.save_cache()
    print(f"cells: {len(cells)}, {conv.cached} unchanged since the last run (cell cache)")
    print(f"skeletons: {len(conv.skeletons)}, "
          f"{sum((OUT / 'cells' / f'skel_{k}.skl').stat().st_size for k in range(len(conv.skeletons))) // 1024} KB")

    game, voices, errors = build_game.build(db, cells, actor_records, actor_cells)
    # Travel services: keep the destinations inside the level, named like doors are
    for a in game["actors"]:
        dests = []
        for pos, rot, cell in a.pop("travel", []):
            if cell is not None:
                if cell in names:
                    dests.append({"cell": cell, "pos": pos, "rot": rot})
                continue
            xy = (int(pos[0] // CELL_SIZE), int(pos[1] // CELL_SIZE))
            if xy in grid and x0 <= xy[0] <= x1 and y0 <= xy[1] <= y1:
                dests.append({"cell": grid[xy]["name"], "grid": list(xy), "pos": pos, "rot": rot})
        if dests:
            a["travel"] = dests
    print(f"  travel: {sum(len(a.get('travel', [])) for a in game['actors'])} destinations inside the level "
          f"({', '.join(sorted({d['cell'] for a in game['actors'] for d in a.get('travel', [])}))})")
    game["chargen"] = chargen
    # Where a game started in a town (past character creation) puts the player: in front of the
    # town door nearest the middle of that town's own cells
    game["spawn"] = spawns
    # Day and night: the weather's colours at sunrise, day, sunset and night (Morrowind.ini)
    wkv = dict(WEATHER_DEFAULTS, **ini_section(ini, f"Weather {args.weather}"))
    rgb = lambda s: [round(int(c) / 255.0, 4) for c in s.split(",")[:3]]
    game["weather"] = {k.lower(): [rgb(wkv.get(f"{k} {t} Color", wkv.get(f"{k} Day Color", "128,128,128")))
                                   for t in ("Sunrise", "Day", "Sunset", "Night")]
                       for k in ("Sky", "Fog", "Ambient", "Sun")}
    # Every weather (REGN WEAT order): its colours, how thick its fog is by day / night, its ambient loop
    game["weather_types"] = []
    for wname in WEATHER_NAMES:
        sec = dict(WEATHER_DEFAULTS, **ini_section(ini, f"Weather {wname}"))
        game["weather_types"].append({
            "name": wname,
            **{k.lower(): [rgb(sec.get(f"{k} {t} Color", sec.get(f"{k} Day Color", "128,128,128")))
                           for t in ("Sunrise", "Day", "Sunset", "Night")] for k in ("Sky", "Fog", "Ambient", "Sun")},
            # (not "fog": that key holds the fog colours above, and this overwrote them: black fog)
            "fog_depth": [float(sec.get("Land Fog Day Depth", "0.69")), float(sec.get("Land Fog Night Depth", "0.69"))],
            "sound": next((sec[k].lower() for k in ("Ambient Loop Sound ID", "Rain Loop Sound ID")
                           if sec.get(k, "None").lower() not in ("", "none")), ""),
            # what OpenMW's Weather class reads besides colours (weather.cpp): transition speed (Hz, real time),
            # wind, glare view, rain threshold, thunder, clouds maximum, rain falling
            **{key: ini_float(sec, name, WEATHER_VALUES[wname][i]) for i, (key, name) in enumerate(WEATHER_KEYS)},
        })
    wsec = ini_section(ini, "Weather")
    game["weather_hours"] = ini_float(wsec, "Hours Between Weather Changes", 20.0)
    # the day's boundaries and how far each colour channel leads / lags them (order sky, fog, ambient, sun)
    game["weather_clock"] = {
        "sunrise": ini_float(wsec, "Sunrise Time", 6.0), "sunset": ini_float(wsec, "Sunset Time", 18.0),
        "sunrise_duration": ini_float(wsec, "Sunrise Duration", 2.0), "sunset_duration": ini_float(wsec, "Sunset Duration", 2.0),
        "windows": [[ini_float(wsec, f"{ch} {p}", d) for p, d in (("Pre-Sunrise Time", 0.0), ("Post-Sunrise Time", 0.0),
                                                                   ("Pre-Sunset Time", 0.0), ("Post-Sunset Time", 0.0))]
                    for ch in ("Sky", "Fog", "Ambient", "Sun")]}
    # Regions: their weather chances (percent, same order); exterior cells say which region they're in
    game["regions"] = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"REGN"}):
        w = r.get("WEAT") or b""
        # ambient sounds (SNAM: sound id, chance percent): birds, wind, insects
        amb = [[d[:32].split(b"\0", 1)[0].decode("latin-1").lower(), d[32]] for t, d in r.subs if t == "SNAM" and len(d) >= 33]
        game["regions"][r.id.lower()] = {"name": r.zstr("FNAM") or r.id, "chances": list(w),
                                         "sleep": (r.zstr("BNAM") or "").lower(), "ambient": amb}
        for sid, _ in amb:
            rec = db["SOUN"].get(sid)
            if rec is not None and sid not in game["sounds"]:
                d = rec.get("DATA")
                game["sounds"][sid] = {"file": rec.zstr("FNAM"), "volume": d[0] / 255.0, "min": d[1], "max": d[2]}
    # Leveled creature lists (placed creatures and sleep interruptions pick from them by the player's
    # level at run time): "all": any entry up to that level (else only the highest such), "none": the
    # chance (percent) of nothing
    game["leveled"] = {}
    for r in read_records(DATA_FILES / "Morrowind.esm", {"LEVC"}):
        flags = struct.unpack("<i", r.get("DATA"))[0] if r.get("DATA") else 0
        game["leveled"][r.id.lower()] = {"all": flags & 1, "none": (r.get("NNAM") or b"\0")[0],
                                         "e": [[lvl, cid.lower()] for lvl, cid in db["LEVC"].get(r.id.lower(), [])]}
    for c in game["cells"]:
        rec = db["CELL"].get(tuple(c["grid"])) if "grid" in c else None
        if rec is not None and rec.zstr("RGNN"):
            c["region"] = rec.zstr("RGNN").lower()
    # their ambient loops (rain, ash storm, blight) and thunder
    for sid in {t["sound"] for t in game["weather_types"] if t["sound"]} | {f"thunder{k}" for k in range(4)}:
        rec = db["SOUN"].get(sid)
        if rec is not None and sid not in game["sounds"]:
            d = rec.get("DATA")
            game["sounds"][sid] = {"file": rec.zstr("FNAM"), "volume": d[0] / 255.0, "min": d[1], "max": d[2]}
    # Divine Intervention (Imperial shrines) and Almsivi Intervention (Tribunal temples) land at these
    # Jail: the prison markers (in towns) send a released player to their destination
    game["markers"] = {"divine": [], "temple": [], "prison": []}
    for xy, rec in db["CELL"].items():
        if isinstance(xy, tuple):
            for r in cell_refs(rec):
                if r["id"].lower() == "prisonmarker" and r.get("dest_cell") in names:
                    game["markers"]["prison"].append({"pos": [*map(float, r["pos"])], "cell": r["dest_cell"],
                                                      "dest": [*map(float, r["dest"][:3]), float(r["dest"][5])]})
                kind = {"divinemarker": "divine", "templemarker": "temple"}.get(r["id"].lower())
                if kind:
                    game["markers"][kind].append([*map(float, r["pos"]), float(r["rot"][2])])
    # Test harness only (GOTO): where a door into each interior cell lands (cell_entries.txt, not game.json)
    entries = {}
    for key, rec in db["CELL"].items():
        for r in cell_refs(rec):
            dc = r.get("dest_cell")
            if dc and r.get("dest") and dc.lower() not in entries:
                d = r["dest"]
                entries[dc.lower()] = f"{dc.lower()}	{d[0]:.0f} {d[1]:.0f} {d[2]:.0f} {d[5]:.3f}"
    (OUT / "cell_entries.txt").write_text("\n".join(entries.values()) + "\n", encoding="latin-1")
    print(f"  cell entries (tests): {len(entries)}")
    # Each actor's mesh library and skeleton: an NPC moved to another cell (PositionCell) is drawn from them
    for a, rec in zip(game["actors"], actor_records):
        skel = conv.actor_skeleton.get(rec.id.lower())
        if skel is not None:
            a["lib"], a["skel"] = conv.library_path(rec.id.lower()), conv.skel_index[skel]
    game["town_spawns"] = {}
    for t in towns:
        tc = town_cells(db, t)
        if tc:
            rect = (min(x for x, _ in tc), min(y for _, y in tc), max(x for x, _ in tc), max(y for _, y in tc))
            sp = street_spawn(db, t, rect)
            if sp:
                game["town_spawns"][t.lower()] = [*sp[0], sp[1]]
    print(f"  town spawns: {game['town_spawns']}")
    game["cell"] = cells[0]["file"]
    print(f"  water ambience: {game['ambient']['water'] or 'none found'}, "
          f"starting kit: {len(game['start_items'])} items + {game['skip_chargen_items']}")
    for e in errors:
        print("  warning:", e)
    # First-person arms and held items (before the textures are encoded: they ask for textures too)
    fp = FirstPerson(db, arch, textures, OUT / "fp")
    game["firstperson"] = fp.build(game["objects"])
    print(f"first person: {len(game['firstperson']['items'])} items, {fp.write() // 1024} KB")
    game["quiz"] = class_quiz(ini)
    game["levelup"] = ini_section(ini, "Level Up")      # Level2 .. Level20, Default: the level-up texts
    # Every named place outdoors gets a label; the 3DS shows those visited or revealed (ShowMap)
    places = sorted({rec.id for xy, rec in db["CELL"].items() if isinstance(xy, tuple) and rec.id})
    game["map"] = build_map(db, arch, terrain, town_labels(db, places))
    # The sky's bodies: the sun (and its glare), Masser and Secunda in their 8 phases (full, waning to new,
    # waxing back), the stars; Morrowind.ini's [Moons] and sunrise / sunset times drive them
    phases = ["full", "three_wan", "half_wan", "one_wan", "new", "one_wax", "half_wax", "three_wax"]
    sky_tex = lambda n: textures.request("textures\\" + n + ".dds", 128, True) if arch.exists("textures\\" + n + ".dds") else ""
    moons = ini_section(ini, "Moons")
    wsec = ini_section(ini, "Weather")
    game["sky_bodies"] = {
        "sun": sky_tex("tx_sun_05"), "glare": sky_tex("tx_sun_flash_grey_05"), "stars": sky_tex("tx_stars"),
        "masser": [sky_tex("tx_masser_" + p) for p in phases], "secunda": [sky_tex("tx_secunda_" + p) for p in phases],
        "moons": {m: {k: float(moons.get(f"{m.capitalize()} {k}", "0") or 0) for k in
                      ("Size", "Fade In Start", "Fade In Finish", "Fade Out Start", "Fade Out Finish", "Axis Offset",
                       "Speed", "Daily Increment")} for m in ("masser", "secunda")},
        "sunrise": float(wsec.get("Sunrise Time", "6")), "sunset": float(wsec.get("Sunset Time", "18")),
    }
    # Distant land past the loaded cells (tools/distant.py)
    wx = weather(ini, args.weather)
    dl = build_distant(db, arch, terrain, EXTERIOR_SUN_DIR, wx["sun"], wx["ambient"], OUT / "distant.bin")
    compress_file(OUT / "distant.bin")
    print(f"  distant land: {dl['cells']} cells, {dl['samples']} samples")
    print(f"  map: {game['map']['width']} x {game['map']['height']} px, {game['map']['units_per_pixel']:.0f} units per pixel")
    game["ui"], report = build_ui(arch, ini_section(ini, "Font Color"), game["objects"])
    game["art"] = build_art(arch, DATA_FILES.parent if DATA_FILES.suffix else DATA_FILES)
    game["movies"] = {k: {"file": v["file"], "sound": v["sound"]}
                      for k, v in build_movies(DATA_FILES.parent if DATA_FILES.suffix else DATA_FILES).items()}
    print(f"  art: {len(game['art']['splash'])} loading screens, {len(game['art']['levelup'])} level-up, "
          f"{len(game['art']['bookart'])} book pictures")
    print(" ", report)
    voices += [q["sound"] for q in game["quiz"] if q["sound"]]

    tstats = textures.encode()
    for e in tstats["failed"][:8]:
        print("  texture skipped:", e)
    print(f"textures: {tstats['count']} ({tstats['encoded']} encoded this run), {tstats['bytes'] // 1024} KB "
          f"in memory at screen-sized resolution vs {tstats['full_bytes'] // 1024} KB at a flat {args.max_tex} cap")
    # What the 3DS linear heap holds: one interior, or the 3 x 3 exterior cells around the player
    # (textures shared between them)
    def footprint(cs):
        geo = sum(c["geometry_bytes"] for c in cs)
        tex = sum(tstats["sizes"].get(n, 0) for n in set(n for c in cs for n in c["textures"]))
        return (geo + tex) / 2 ** 20, geo // 1024, tex // 1024
    for c in cells:
        if c["interior"]:
            mb, geo, tex = footprint([c])
            if mb > LINEAR_BUDGET_MB * 0.5:
                print(f"  {c['name']}: geometry {geo} KB + textures {tex} KB = {mb:.1f} MB linear")
    spots = sorted(((footprint([grid[(gx + i, gy + j)] for i in (-1, 0, 1) for j in (-1, 0, 1)
                                if (gx + i, gy + j) in grid]), (gx, gy))
                    for gx in range(x0, x1 + 1) for gy in range(y0, y1 + 1) if (gx, gy) in grid),
                   key=lambda t: -t[0][0])
    # Every spot and interior, heaviest first: build/memory-<data folder>.txt
    report = ROOT / "build" / f"memory-{OUT.name}.txt"
    report.parent.mkdir(exist_ok=True)
    with open(report, "w", encoding="utf-8") as f:
        f.write(f"linear memory per 3x3 exterior spot and per interior (budget {LINEAR_BUDGET_MB} MB)\n\n")
        for (mb, geo, tex), xy in spots[:40]:
            f.write(f"{mb:5.1f} MB  exterior {xy} {grid[xy]['name']}: geometry {geo} KB, textures {tex} KB\n")
        f.write("\n")
        for mb, c in sorted(((footprint([c])[0], c) for c in cells if c["interior"]), key=lambda t: -t[0])[:40]:
            f.write(f"{mb:5.1f} MB  {c['name']}\n")
    print(f"  memory report: {report}")
    worst = spots[0]
    (mb, geo, tex), xy = worst
    flag = "  <-- over the budget: lower --max-tex" if mb > LINEAR_BUDGET_MB else ""
    print(f"  heaviest exterior spot {xy} ({grid[xy]['name']}): 3x3 cells = geometry {geo} KB + textures {tex} KB "
          f"= {mb:.1f} MB linear{flag}")

    # Sounds
    # (sharded in 64 folders: see textures.shard_dir)
    sdir = OUT / "snd"
    migrate_flat(OUT / "sound", sdir, "*.snd")
    if args.force and sdir.exists():
        for p in sdir.rglob("*.snd"):
            p.unlink()
    missing = []
    for sid, s in game["sounds"].items():
        name = convert_sound(arch, "Sound\\" + s["file"], sdir, shard=True)
        s["file"] = name
        if name is None:
            missing.append(sid)
    game["sounds"] = {k: v for k, v in game["sounds"].items() if v["file"]}
    game["voices"] = {}
    print(f"sounds: {len(game['sounds'])} effects, {len(voices)} voice lines (new ones are decoded from MP3)", flush=True)
    for vi, v in enumerate(voices):
        if vi and vi % 100 == 0:
            print(f"  voices {vi}/{len(voices)}", flush=True)
        name = convert_sound(arch, "Sound\\" + v, sdir, VOICE_RATE, shard=True)
        if name:
            game["voices"][v.lower().replace("/", "\\")] = name
        else:
            missing.append(v)
    game["music"] = [n for n in (convert_sound(arch, m, OUT / "music") for m in MUSIC) if n]
    game["music_battle"] = [n for n in (convert_sound(arch, m, OUT / "music") for m in MUSIC_BATTLE) if n]
    if missing:
        print(f"  missing sounds: {missing[:8]}{' ...' if len(missing) > 8 else ''}")
    # Sounds a previous level used but this one doesn't
    used = set(game["voices"].values()) | {s["file"] for s in game["sounds"].values()}
    stale = [p for p in sdir.rglob("*.snd") if p.name not in used]
    for p in stale:
        p.unlink()

    # The big sections go in files of their own: the 3DS parses one at a time (a whole parsed
    # game.json doesn't fit its heap)
    game = finite(game)
    full = dict(game)                     # every section, for the summary below
    for old in OUT.glob("game_*.json"):
        old.unlink()
    split = []
    for key in SPLIT_SECTIONS:
        if key not in game:
            continue
        value = game.pop(key)
        text = json.dumps({key: value}, separators=(",", ":"), allow_nan=False)
        # Big sections in parts (game_<key>_0.json ..): the 3DS parses one at a time, and a parsed
        # section takes several times its size
        parts = max(1, len(text) // PART_BYTES + 1) if isinstance(value, (list, dict)) and len(value) > 1 else 1
        if parts == 1:
            (OUT / f"game_{key}.json").write_text(text)
            split.append(OUT / f"game_{key}.json")
            continue
        items = list(value.items()) if isinstance(value, dict) else value
        chunks, cur, size = [], [], 0
        for item in items:
            n = len(json.dumps(item, separators=(",", ":"), allow_nan=False))
            if cur and size + n > PART_BYTES:
                chunks.append(cur)
                cur, size = [], 0
            cur.append(item)
            size += n
        chunks.append(cur)
        for k, chunk in enumerate(chunks):
            p = OUT / f"game_{key}_{k}.json"
            p.write_text(json.dumps({key: dict(chunk) if isinstance(value, dict) else chunk},
                                    separators=(",", ":"), allow_nan=False))
            split.append(p)
    with open(OUT / "game.json", "w") as f:
        json.dump(game, f, separators=(",", ":"), allow_nan=False)
    sizes = {}
    for p in split:
        name = re.sub(r"_\d+$", "", p.stem[5:])
        sizes[name] = sizes.get(name, 0) + p.stat().st_size // 1024
    sizes = sorted(((v, k) for k, v in sizes.items()), reverse=True)
    print("  game data sections (KB): " + ", ".join(f"{n} {k}" for k, n in [(k, n) for n, k in sizes][:6]))
    packed = [OUT / "game.json", *split, *(p for p in (OUT / "cells").glob("*") if p.is_file()), *(OUT / "cells" / "actors").rglob("*.aml"), *(OUT / "fp").glob("*")]
    before = sum(p.stat().st_size for p in packed)
    for p in packed:
        compress_file(p)
    after = sum(p.stat().st_size for p in packed)
    print(f"zlib: cells + fp + game.json {before // 1024} KB -> {after // 1024} KB")
    size = lambda p: sum(x.stat().st_size for x in p.rglob("*") if x.is_file()) // 1024 if p.exists() else 0
    print(f"game.json {(OUT / 'game.json').stat().st_size // 1024} KB: {len(full['cells'])} cells, "
          f"{len(full['actors'])} actors, {len(full['objects'])} objects, "
          f"{len(full['scripts'])} scripts, {len(full['dialogue'])} topics "
          f"({sum(len(t['infos']) for t in full['dialogue'])} responses), {len(full['journal'])} journals; "
          f"cells {size(OUT / 'cells')} KB, textures {size(OUT / 'tex')} KB, "
          f"sound {size(sdir)} KB ({len(full['sounds'])} fx, {len(full['voices'])} voice), "
          f"music {size(OUT / 'music')} KB; total {time.time() - t0:.0f}s")
    if failed:
        print(f"{len(failed)} cells FAILED to convert (left out):")
        for name, err in failed:
            print(f"  {name}: {err}")
    shard_cells()


def unshard_cells():
    """Cell files back from their subfolders (cells/<xx>/<file>.*) to cells/, where the converter and
    its cache keep them."""
    root = OUT / "cells"
    for d in root.glob("[0-9a-f][0-9a-f]"):
        if d.is_dir():
            for p in d.iterdir():
                p.replace(root / p.name)
            d.rmdir()


def shard_cells():
    """Cell files (.cel / .act / .json) into 64 subfolders by their name (source/datapath.h cellPath):
    the 3DS's SD card scans a whole folder to open or create a file, and the island has ~10000."""
    root = OUT / "cells"
    for p in list(root.iterdir()):
        if p.is_file() and p.suffix in (".cel", ".act", ".json"):
            d = root / shard_dir(p.stem)
            d.mkdir(exist_ok=True)
            p.replace(d / p.name)


if __name__ == "__main__":
    main()
