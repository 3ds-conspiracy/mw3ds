"""Readers for Morrowind's BSA archives and ESM/ESP record files."""
import os
import struct
from pathlib import Path

from envfile import require

MORROWIND_DIR = Path(require("MW3DS_MORROWIND_DIR", "the folder of your Morrowind install (with Morrowind.ini and Data Files)"))
DATA_FILES = MORROWIND_DIR / "Data Files"


class BSA:
    """Morrowind (TES3) BSA archive: flat file table, lowercase backslash paths."""

    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, "rb") as f:
            version, hash_offset, count = struct.unpack("<3I", f.read(12))
            if version != 0x100:
                raise ValueError(f"{path}: not a TES3 BSA (version {version:#x})")
            sizes_offsets = struct.unpack(f"<{count * 2}I", f.read(count * 8))
            name_offsets = struct.unpack(f"<{count}I", f.read(count * 4))
            names_start = f.tell()
            names = f.read(12 + hash_offset - names_start)
        data_start = 12 + hash_offset + count * 8
        self.files = {}
        for i in range(count):
            end = names.index(b"\0", name_offsets[i])
            name = names[name_offsets[i]:end].decode("latin-1").lower()
            self.files[name] = (data_start + sizes_offsets[i * 2 + 1], sizes_offsets[i * 2])

    def read(self, name):
        offset, size = self.files[name.lower().replace("/", "\\")]
        with open(self.path, "rb") as f:
            f.seek(offset)
            return f.read(size)


class Archives:
    """Loose files in Data Files override BSA contents, later BSAs override earlier ones."""

    def __init__(self, bsa_names=("Morrowind.bsa", "Tribunal.bsa", "Bloodmoon.bsa")):
        self.bsas = [BSA(DATA_FILES / n) for n in bsa_names if (DATA_FILES / n).exists()]
        # Loose files by lowercase backslash path, the way the BSAs name theirs: Morrowind's own names are
        # case-blind and use backslashes, which a case-sensitive file system (Linux) or a plain Path join gets wrong
        self.loose = {}
        for root, _, files in os.walk(DATA_FILES):
            rel = Path(root).relative_to(DATA_FILES).parts
            for f in files:
                self.loose["\\".join(rel + (f,)).lower()] = Path(root) / f

    def exists(self, name):
        name = name.lower().replace("/", "\\")
        return name in self.loose or any(name in b.files for b in self.bsas)

    def read(self, name):
        name = name.lower().replace("/", "\\")
        loose = self.loose.get(name)
        if loose:
            return loose.read_bytes()
        for b in reversed(self.bsas):
            if name in b.files:
                return b.read(name)
        raise FileNotFoundError(name)

    def names(self):
        out = set()
        for b in self.bsas:
            out.update(b.files)
        return out


class Record:
    __slots__ = ("tag", "flags", "subs")

    def __init__(self, tag, flags, subs):
        self.tag, self.flags, self.subs = tag, flags, subs

    def get(self, tag):
        for t, d in self.subs:
            if t == tag:
                return d
        return None

    def zstr(self, tag):
        d = self.get(tag)
        return None if d is None else d.split(b"\0", 1)[0].decode("latin-1")

    @property
    def id(self):
        return self.zstr("NAME")


def read_records(path, wanted=None):
    """Yields Records from an ESM/ESP. `wanted` limits parsing to those record tags."""
    with open(path, "rb") as f:
        data = f.read()
    pos, end = 0, len(data)
    while pos < end:
        tag = data[pos:pos + 4].decode("latin-1")
        size, _, flags = struct.unpack_from("<3I", data, pos + 4)
        body_start = pos + 16
        pos = body_start + size
        if wanted and tag not in wanted:
            continue
        subs, p = [], body_start
        while p < pos:
            stag = data[p:p + 4].decode("latin-1")
            (ssize,) = struct.unpack_from("<I", data, p + 4)
            subs.append((stag, data[p + 8:p + 8 + ssize]))
            p += 8 + ssize
        yield Record(tag, flags, subs)


# Record types with a MODL we can place in a cell as a static mesh (first pass: no NPCs)
MODEL_TYPES = {"STAT", "DOOR", "ACTI", "CONT", "LIGH", "MISC", "WEAP", "ARMO", "CLOT",
               "BOOK", "ALCH", "APPA", "INGR", "LOCK", "PROB", "REPA", "CREA", "BODY"}


def cell_refs(cell):
    """Splits a CELL record's subrecords into placed references."""
    refs, cur = [], None
    for tag, d in cell.subs:
        if tag == "FRMR":
            cur = {"id": None, "scale": 1.0, "pos": None, "rot": None, "deleted": False}
            refs.append(cur)
        elif cur is None:
            continue
        elif tag == "NAME":
            cur["id"] = d.split(b"\0", 1)[0].decode("latin-1")
        elif tag == "XSCL":
            cur["scale"] = struct.unpack("<f", d)[0]
        elif tag == "DATA":
            v = struct.unpack("<6f", d)
            cur["pos"], cur["rot"] = v[:3], v[3:]
        elif tag == "DELE":
            cur["deleted"] = True
        elif tag == "DODT":
            cur["dest"] = struct.unpack("<6f", d)
        elif tag == "DNAM":
            cur["dest_cell"] = d.split(b"\0", 1)[0].decode("latin-1")
        elif tag == "FLTV":
            cur["lock"] = struct.unpack("<i", d)[0]
        elif tag == "KNAM":
            cur["key"] = d.split(b"\0", 1)[0].decode("latin-1")
        elif tag == "NAM9":
            cur["count"] = struct.unpack("<i", d)[0]
        elif tag == "XSOL":
            cur["soul"] = d.split(b"\0", 1)[0].decode("latin-1").lower()     # a filled soul gem lying about
        elif tag == "XCHG":
            cur["charge"] = struct.unpack("<f", d)[0]                           # an enchanted item's charge
    return [r for r in refs if r["pos"] and not r["deleted"]]


def load_db(path=None):
    """One pass over Morrowind.esm collecting everything the level tools use."""
    db = {"objects": {}, "SCPT": {}, "GLOB": {}, "GMST": {}, "CLAS": {}, "RACE": {}, "BSGN": {},
          "SPEL": {}, "SOUN": {}, "SKIL": {}, "CELL": {}, "FACT": {}, "LAND": {}, "LTEX": {}, "REGN": {},
          "LEVC": {}, "LEVI": {}, "MGEF": {}, "PGRD": {}, "dialogue": []}
    dial = None
    skip = {"SNDG", "ENCH", "TES3"}
    for r in read_records(path or DATA_FILES / "Morrowind.esm"):
        t = r.tag
        if t in skip:
            continue
        if t == "DIAL":
            dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}
            db["dialogue"].append(dial)
        elif t == "INFO":
            dial["infos"].append(r)
        elif t == "SCPT":
            name = r.get("SCHD")[:32].split(b"\0", 1)[0].decode("latin-1").lower()
            db["SCPT"][name] = r.zstr("SCTX") or ""
        elif t == "GLOB":
            v = r.get("FLTV")
            db["GLOB"][r.id.lower()] = (r.zstr("FNAM"), struct.unpack("<f", v)[0] if v else 0.0)
        elif t == "GMST":
            if r.get("STRV") is not None:
                val = r.get("STRV").split(b"\0", 1)[0].decode("latin-1")
            elif r.get("INTV") is not None:
                val = struct.unpack("<i", r.get("INTV"))[0]
            elif r.get("FLTV") is not None:
                val = struct.unpack("<f", r.get("FLTV"))[0]
            else:
                val = ""
            db["GMST"][r.id.lower()] = val
        elif t == "SKIL":
            db["SKIL"][struct.unpack("<i", r.get("INDX"))[0]] = r
        elif t == "CELL":
            if struct.unpack("<I", r.get("DATA")[:4])[0] & 1:
                db["CELL"][r.id] = r
            else:
                db["CELL"][struct.unpack("<ii", r.get("DATA")[4:12])] = r
        elif t == "PGRD":
            # Path grid: points (x, y, z; exterior ones relative to the cell's corner) and edges
            gx, gy = struct.unpack_from("<ii", r.get("DATA"))
            pts = r.get("PGRP") or b""
            points = [struct.unpack_from("<3iBBH", pts, i * 16) for i in range(len(pts) // 16)]
            conns = r.get("PGRC") or b""
            targets = struct.unpack(f"<{len(conns) // 4}I", conns)
            edges, k = [], 0
            for i, p in enumerate(points):
                for t2 in targets[k:k + p[4]]:
                    if t2 < len(points) and i < t2:
                        edges.append((i, t2))
                k += p[4]
            name = r.zstr("NAME") or ""
            key = (gx, gy) if (gx or gy or not name) else name
            if key in db["PGRD"] and key == (0, 0) and name:
                key = name
            db["PGRD"][key] = ([p[:3] for p in points], edges)
        elif t == "MGEF":
            # magic effect: school (0 alteration .. 5 restoration), base cost
            school, cost = struct.unpack_from("<if", r.get("MEDT"))
            db["MGEF"][struct.unpack("<i", r.get("INDX"))[0]] = {"school": school, "cost": cost}
        elif t in ("LEVC", "LEVI"):
            # (level, creature / item or nested list id) pairs: CNAM / INAM then INTV
            entries, cur = [], None
            for tag, d in r.subs:
                if tag in ("CNAM", "INAM"):
                    cur = d.split(b"\0", 1)[0].decode("latin-1")
                elif tag == "INTV" and cur is not None:
                    entries.append((struct.unpack("<H", d[:2])[0], cur))
                    cur = None
            db[t][r.id.lower()] = entries
        elif t == "REGN":
            db["REGN"][r.id.lower()] = r.zstr("FNAM") or r.id
        elif t == "LAND":
            db["LAND"][struct.unpack("<ii", r.get("INTV"))] = r
        elif t == "LTEX":
            # VTEX values are LTEX index + 1 (0 = the default land texture)
            db["LTEX"][struct.unpack("<i", r.get("INTV"))[0]] = r.zstr("DATA")
        elif t in ("CLAS", "RACE", "BSGN", "SPEL", "SOUN", "FACT"):
            db[t][r.id.lower()] = r
        elif r.id:
            db["objects"][r.id.lower()] = r
    return db
