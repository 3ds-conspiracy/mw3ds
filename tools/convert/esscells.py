"""What the PC game's saves and the engine's saves both need to name an object in the world: Morrowind's own
reference number (FRMR, by cell) against the engine's (the cell file and the reference's place in that cell's
converted list), and the scripts' local variables in the two orders they are kept in.

Used by ess.py (PC save to 3DS) and ess_export.py (3DS save to PC). The converted cells don't hold Morrowind's
numbers, so each reference is found again by its id and where it stands, which the conversion copies unchanged.
"""
import json
import struct
import zlib
from pathlib import Path

from mwfiles import masters, read_records, safe_name, text

CELL_SIZE = 8192
ITEM_TYPES = {"ALCH", "APPA", "ARMO", "BOOK", "CLOT", "INGR", "LIGH", "LOCK", "MISC", "PROB", "REPA", "WEAP"}


def read_json(path):
    raw = Path(path).read_bytes()
    return json.loads(zlib.decompress(raw[8:]) if raw[:4] == b"MWZ1" else raw)


def exterior_file(gx, gy):
    return "ext_%s%d_%s%d" % ("m" if gx < 0 else "", abs(gx), "m" if gy < 0 else "", abs(gy))


def cell_file(name, flags, gx, gy):
    """The engine's cell file for a CELL record: interiors by name, the exterior by its grid square."""
    return safe_name(name) if flags & 1 else exterior_file(gx, gy)


def ref_groups(subs):
    """A CELL record's references: one list of (tag, data) per FRMR, with the FRMR value first."""
    groups, cur = [], None
    for t, v in subs:
        if t == "FRMR":
            cur = [struct.unpack("<I", v)[0]]
            groups.append(cur)
        elif cur is not None:
            cur.append((t, v))
    return groups


def sub(group, tag):
    for t, v in group[1:]:
        if t == tag:
            return v
    return None


ADDONS = ("Tribunal.esm", "Bloodmoon.esm")      # in the load order the converters use, after Morrowind.esm


def data_addons(data, esm):
    """The add-ons the converted game was made with (game.json's "addons") that are beside Morrowind.esm."""
    built = {a.lower() + ".esm" for a in read_json(Path(data) / "game.json").get("addons", [])}
    return [a for a in ADDONS if a.lower() in built and Path(esm).with_name(a).is_file()]


def ref_key(content, num):
    """World's key for reference `num` of the file read `content`-th (0: Morrowind.esm, keyed by the number alone)."""
    return (content << 24 | num) if content else num


def save_contents(world, masters):
    """A save's FRMR top byte (its masters, 1 first) -> the World file it names; masters World didn't read are left
    out."""
    return {k + 1: world.contents.index(m.lower()) for k, m in enumerate(masters) if m.lower() in world.contents}


class World:
    """Morrowind.esm's cells and references (and the add-ons' named in `addons`, laid over them in load order), and
    the converted cells' references, linked object by object.

    A reference is known by its number in the file it comes from: Morrowind.esm's by the number alone (as ess.py
    looks them up), an add-on's by (its place in the files read) << 24 | number, which is also ref["content"].
    Tribunal and Bloodmoon add references of their own only (none of Morrowind.esm's is changed, moved or deleted
    by them), so the two kinds never meet."""

    def __init__(self, esm, data, addons=()):
        self.data = Path(data)
        self.cells = {}              # file -> {"name", "interior", "grid", "region", "refs": {key: ref}}
        self.region_names = {}
        self.where = {}              # reference key -> cell file
        self.leveled = set()         # levelled creature lists: a reference to one is a spawn point
        self.addon_names = {}        # cell file -> name, for the add-ons not read in full (a save made there names it)
        files = [Path(esm)] + [Path(esm).with_name(n) for n in addons]
        self.file_names = [f.name for f in files]
        self.contents = [n.lower() for n in self.file_names]
        for content, path in enumerate(files):
            parents = [next((k for k, f in enumerate(files) if f.name.lower() == m.lower()), None)
                       for m in masters(path)] if content else []
            for r in read_records(path, {"CELL", "REGN", "LEVC"}):
                if r.tag == "LEVC":
                    self.leveled.add(r.id.lower())
                    continue
                if r.tag == "REGN":
                    self.region_names[r.id.lower()] = r.zstr("FNAM") or r.id
                    continue
                d = r.get("DATA")
                flags, gx, gy = struct.unpack_from("<Iii", d)
                name = r.id or ""
                file = cell_file(name, flags, gx, gy)
                cell = self.cells.get(file)
                if cell is None:
                    cell = self.cells[file] = {"name": name, "interior": bool(flags & 1), "grid": (gx, gy), "data": d,
                                               "region": (r.zstr("RGNN") or "").lower(), "refs": {}}
                elif not cell["region"] and r.zstr("RGNN"):
                    cell["region"] = r.zstr("RGNN").lower()
                for g in ref_groups(r.subs):
                    local = g[0] >> 24
                    owner = parents[local - 1] if 0 < local <= len(parents) else content
                    if owner is None:
                        continue                 # (a master the files read don't include)
                    key = ref_key(owner, g[0] & 0xFFFFFF)
                    if sub(g, "DELE") is not None:
                        cell["refs"].pop(key, None)
                        continue
                    dat = sub(g, "DATA")
                    if dat is None:
                        continue
                    pos = struct.unpack("<6f", dat)
                    lock = sub(g, "FLTV")
                    cell["refs"][key] = {"id": text(sub(g, "NAME")).lower(), "pos": pos[:3], "rot": pos[3:],
                                         "lock": struct.unpack("<i", lock)[0] if lock else 0,
                                         "trap": bool(sub(g, "TNAM")), "frmr": g[0] & 0xFFFFFF, "content": owner,
                                         # (NAM0, the count of temporary references, ends the persistent ones: not
                                         # the reference's own)
                                         "raw": [s for s in g[1:] if s[0] != "NAM0"]}
                    self.where[key] = file
        for extra in ADDONS:
            path = Path(esm).with_name(extra)
            if extra in addons or not path.is_file():
                continue
            for r in read_records(path, {"CELL"}):
                flags, gx, gy = struct.unpack_from("<Iii", r.get("DATA"))
                if r.id and flags & 1:
                    self.addon_names.setdefault(cell_file(r.id, flags, gx, gy), r.id)
        self.paths = {p.stem: p for p in (self.data / "cells").glob("*/*.json")}
        self._links = {}

    def display_name(self, file):
        """The cell's name as the PC game shows it: an exterior without one goes by its region."""
        c = self.cells.get(file)
        if not c:
            return self.addon_names.get(file, "")
        if c["name"] or c["interior"]:
            return c["name"]
        return self.region_names.get(c["region"], "Wilderness")

    def ours(self, file):
        """The converted cell's references (id, pos), in the engine's order."""
        p = self.paths.get(file)
        return [(r["id"].lower(), r["pos"]) for r in read_json(p)["refs"]] if p else []

    def link(self, file):
        """(Morrowind index -> engine index, engine index -> Morrowind index) for one cell."""
        if file in self._links:
            return self._links[file]
        ours = self.ours(file)
        by_id = {}
        for i, (rid, pos) in enumerate(ours):
            by_id.setdefault(rid, []).append((i, pos))
        to_ours, to_mw = {}, {}
        esm_refs = self.cells.get(file, {}).get("refs", {})
        for idx, r in esm_refs.items():
            best, bd = None, 1.0
            for i, pos in by_id.get(r["id"], []):
                d = max(abs(a - b) for a, b in zip(pos, r["pos"]))
                if d < bd and i not in to_mw:
                    best, bd = i, d
            if best is None:
                # a levelled creature's spawn point: the conversion put the creature it rolled in its place
                for i, (rid, pos) in enumerate(ours):
                    if i not in to_mw and max(abs(a - b) for a, b in zip(pos, r["pos"])) < 1.0:
                        best = i
                        break
            if best is not None:
                to_ours[idx], to_mw[best] = best, idx
        self._links[file] = (to_ours, to_mw)
        return to_ours, to_mw


def read_engine_save(path):
    """An engine save (save.json / save_NNN.json): its JSON, with the refs and scripts of the cells kept out of
    memory (version 2's 'MWC1' blocks after the JSON) added to the JSON's own."""
    raw = Path(path).read_bytes()
    if raw[:4] != b"MWZ1":
        return json.loads(raw)
    z = zlib.decompressobj()
    root = json.loads(z.decompress(raw[8:]))
    rest = z.unused_data
    if rest[:4] == b"MWC1":
        (count,), p = struct.unpack_from("<I", rest, 4), 8
        for _ in range(count):
            (nlen,) = struct.unpack_from("<H", rest, p)
            p += 2 + nlen + 1
            raw_len, zlen = struct.unpack_from("<II", rest, p)
            p += 8
            if zlen:
                kept = json.loads(zlib.decompress(rest[p:p + zlen]))
                root.setdefault("refs", []).extend(kept.get("refs", []))
                root.setdefault("scripts", []).extend(kept.get("scripts", []))
            p += zlen
    return root


def script_tables(data, esm, addons=()):
    """Script name (lower case) -> {"name", "locals": [(type, name)], "schd": SCHD sizes, "mw_locals": (shorts,
    longs, floats) by name}. SCHD and the local names are the last file's of Morrowind.esm and `addons` (the
    version the PC game runs: an add-on that rewrites a script may give it other locals)."""
    out = {}
    for p in sorted(Path(data).glob("game_scripts_*.json")):
        for k, s in read_json(p)["scripts"].items():
            out[k.lower()] = {"name": s["name"], "locals": [tuple(l) for l in s["locals"]]}
    for path in [Path(esm)] + [Path(esm).with_name(n) for n in addons]:
        for r in read_records(path, {"SCPT"}):
            h = r.get("SCHD")
            name = text(h[:32]).lower()
            if name not in out:
                continue
            ns, nl, nf = struct.unpack_from("<3i", h, 32)
            names = [text(n).lower() for n in (r.get("SCVR") or b"").split(b"\0")]
            names = (names + [""] * (ns + nl + nf))[:ns + nl + nf]
            out[name]["schd"] = h[32:52]
            out[name]["mw_locals"] = (names[:ns], names[ns:ns + nl], names[ns + nl:])
    return out


def locals_from_mw(script, shorts, longs, floats):
    """Morrowind keeps a script's shorts, longs and floats apart, each in the order declared; the engine keeps one
    list in the order declared."""
    left = {"short": list(shorts), "long": list(longs), "float": list(floats)}
    return [float(left[t].pop(0)) if left.get(t) else 0.0 for t, _ in script["locals"]]


def _same_locals(script):
    """The PC game's version of the script declares the engine's locals (or isn't known): by type and order then."""
    pc = script.get("mw_locals")
    if pc is None:
        return True
    ours = {"short": [], "long": [], "float": []}
    for t, n in script["locals"]:
        ours.setdefault(t, []).append(n.lower())
    return (ours["short"], ours["long"], ours["float"]) == tuple(pc)


def locals_from_pc(script, shorts, longs, floats):
    """locals_from_mw by name when the PC game's version of the script is another (script_tables' "mw_locals")."""
    pc = script.get("mw_locals")
    if _same_locals(script):
        return locals_from_mw(script, shorts, longs, floats)
    vals = {}
    for names, got in zip(pc, (shorts, longs, floats)):
        for n, v in zip(names, got):
            vals.setdefault(n, float(v))
    return [vals.get(n.lower(), 0.0) for _, n in script["locals"]]


def locals_to_mw(script, values):
    split = {"short": [], "long": [], "float": []}
    for (t, _), v in zip(script["locals"], list(values) + [0.0] * len(script["locals"])):
        split.setdefault(t, []).append(v)
    return split["short"], split["long"], split["float"]


def locals_to_pc(script, values):
    """The engine's locals as the PC game's version of the script declares them (script_tables' "mw_locals"),
    matched by name: one the engine's version doesn't have starts at 0. The same version: by type and order."""
    pc = script.get("mw_locals")
    if _same_locals(script):
        return locals_to_mw(script, values)
    vals = {n.lower(): v for (_, n), v in zip(script["locals"], list(values) + [0.0] * len(script["locals"]))}
    return tuple([vals.get(n, 0.0) for n in group] for group in pc)


def read_scri(group_or_subs, start=0):
    """SCRI / SLCS / SLSD / SLLD / SLFD from a list of (tag, data): (script, shorts, longs, floats) or None."""
    items = group_or_subs[start:]
    name, counts, vals = None, (0, 0, 0), {}
    for t, v in items:
        if not isinstance(t, str):
            continue
        if t == "SCRI":
            name = text(v)
        elif t == "SLCS":
            counts = struct.unpack_from("<3i", v)
        elif t == "SLSD":
            vals["s"] = struct.unpack_from("<%dh" % counts[0], v)
        elif t == "SLLD":
            vals["l"] = struct.unpack_from("<%di" % counts[1], v)
        elif t == "SLFD":
            vals["f"] = struct.unpack_from("<%df" % counts[2], v)
    if name is None and not vals:
        return None
    return name, vals.get("s", ()), vals.get("l", ()), vals.get("f", ())


def scri_subs(name, shorts, longs, floats):
    """The subrecords Morrowind writes for a script's locals (a reference's, an item's or a global script's)."""
    out = [("SCRI", name.encode("latin-1") + b"\0")] if name else []
    out.append(("SLCS", struct.pack("<3i", len(shorts), len(longs), len(floats))))
    if shorts:
        out.append(("SLSD", struct.pack("<%dh" % len(shorts), *[max(-32768, min(32767, int(round(v)))) for v in shorts])))
    if longs:
        out.append(("SLLD", struct.pack("<%di" % len(longs), *[int(round(v)) for v in longs])))
    if floats:
        out.append(("SLFD", struct.pack("<%df" % len(floats), *floats)))
    return out
