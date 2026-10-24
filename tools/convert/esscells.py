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

from mwfiles import read_records, safe_name, text

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


class World:
    """Morrowind.esm's cells and references, and the converted cells' references, linked object by object."""

    def __init__(self, esm, data):
        self.data = Path(data)
        self.cells = {}              # file -> {"name", "interior", "grid", "region", "refs": {index: ref}}
        self.region_names = {}
        self.where = {}              # Morrowind index -> cell file (an index is unique in Morrowind.esm)
        self.leveled = set()         # levelled creature lists: a reference to one is a spawn point
        for r in read_records(esm, {"CELL", "REGN", "LEVC"}):
            if r.tag == "LEVC":
                self.leveled.add(r.id.lower())
                continue
            if r.tag == "REGN":
                self.region_names[r.id.lower()] = r.zstr("FNAM") or r.id
                continue
            d = r.get("DATA")
            flags, gx, gy = struct.unpack_from("<Iii", d)
            name = r.id or ""
            refs = {}
            for g in ref_groups(r.subs):
                if sub(g, "DELE") is not None:
                    continue
                dat = sub(g, "DATA")
                if dat is None:
                    continue
                pos = struct.unpack("<6f", dat)
                lock = sub(g, "FLTV")
                refs[g[0] & 0xFFFFFF] = {"id": text(sub(g, "NAME")).lower(), "pos": pos[:3], "rot": pos[3:],
                                         "lock": struct.unpack("<i", lock)[0] if lock else 0,
                                         "trap": bool(sub(g, "TNAM")), "frmr": g[0] & 0xFFFFFF, "raw": g[1:]}
            for idx in refs:
                self.where[idx] = cell_file(name, flags, gx, gy)
            self.cells[cell_file(name, flags, gx, gy)] = {
                "name": name, "interior": bool(flags & 1), "grid": (gx, gy), "data": d,
                "region": (r.zstr("RGNN") or "").lower(), "refs": refs}
        self.paths = {p.stem: p for p in (self.data / "cells").glob("*/*.json")}
        self._links = {}

    def display_name(self, file):
        """The cell's name as the PC game shows it: an exterior without one goes by its region."""
        c = self.cells.get(file)
        if not c:
            return ""
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


def script_tables(data, esm):
    """Script name (lower case) -> {"name", "locals": [(type, name)], "schd": SCHD sizes from Morrowind.esm}."""
    out = {}
    for p in sorted(Path(data).glob("game_scripts_*.json")):
        for k, s in read_json(p)["scripts"].items():
            out[k.lower()] = {"name": s["name"], "locals": [tuple(l) for l in s["locals"]]}
    for r in read_records(esm, {"SCPT"}):
        h = r.get("SCHD")
        name = text(h[:32]).lower()
        if name in out:
            out[name]["schd"] = h[32:52]
    return out


def locals_from_mw(script, shorts, longs, floats):
    """Morrowind keeps a script's shorts, longs and floats apart, each in the order declared; the engine keeps one
    list in the order declared."""
    left = {"short": list(shorts), "long": list(longs), "float": list(floats)}
    return [float(left[t].pop(0)) if left.get(t) else 0.0 for t, _ in script["locals"]]


def locals_to_mw(script, values):
    split = {"short": [], "long": [], "float": []}
    for (t, _), v in zip(script["locals"], list(values) + [0.0] * len(script["locals"])):
        split.setdefault(t, []).append(v)
    return split["short"], split["long"], split["float"]


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
