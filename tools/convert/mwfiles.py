"""Readers for Morrowind's BSA archives and ESM/ESP record files.

The game's records come from Morrowind.esm and the plugins in MW3DS_PLUGINS (";"-separated .esp / .esm names in
Data Files, in load order), merged as OpenMW merges them: read_game() and load_db() give the merged records,
read_records() one file as it is. Text in them is in the release's code page, MW3DS_ENCODING (OpenMW's --encoding
names): win1252 (English, German, French ...), win1250 (Polish, Czech ...) or win1251 (Russian).
"""
import hashlib
import re
import struct
import zlib
from pathlib import Path

import os

from envfile import require

MORROWIND_DIR = Path(require("MW3DS_MORROWIND_DIR", "the folder of your Morrowind install (with Morrowind.ini and Data Files)"))
DATA_FILES = MORROWIND_DIR / "Data Files"
# Extra mod folders laid over Data Files (";"-separated, later ones win), each laid out like Data Files
# (meshes\..., textures\...). Nothing is copied into the Morrowind install.
OVERLAYS = [Path(p) for p in os.environ.get("MW3DS_OVERLAY_DIRS", "").split(";") if p.strip()]

ENCODINGS = {"win1252": "cp1252", "win1250": "cp1250", "win1251": "cp1251"}
ENCODING = (os.environ.get("MW3DS_ENCODING") or "win1252").strip().lower()
if ENCODING not in ENCODINGS:
    raise SystemExit(f"MW3DS_ENCODING={ENCODING}: use one of {', '.join(ENCODINGS)}")
CODEC = ENCODINGS[ENCODING]

MASTER = "Morrowind.esm"
# Masters this project leaves out: a plugin that needs one is refused
OUT_OF_SCOPE = {"tribunal.esm", "bloodmoon.esm"}
PLUGINS = [p.strip() for p in os.environ.get("MW3DS_PLUGINS", "").split(";") if p.strip()]


def text(d):
    """Game text (an ID, a name, dialogue, script source) from record bytes, up to the first NUL."""
    return d.split(b"\0", 1)[0].decode(CODEC, errors="replace")


def text_bytes(s):
    """Back to the bytes the record held (for hashes that must not change with the encoding setting)."""
    return s.encode(CODEC, errors="replace")


def safe_name(s):
    """A file name from an ID or cell name: lower case, [a-z0-9_] kept, anything else '_'. A name with letters
    outside ASCII (Polish, Cyrillic ...) also gets a short hash of itself, or many would come out the same."""
    s = s.lower()
    out = re.sub(r"[^a-z0-9_]", "_", s)
    if any(ord(c) > 127 for c in s):
        out += "_%08x" % zlib.crc32(s.encode("utf-8"))
    return out


def fixed_utf8(s, size):
    """s as UTF-8 in a NUL-padded field of `size` bytes, cut at a whole character."""
    b = s.encode("utf-8")[:size - 1].decode("utf-8", errors="ignore").encode("utf-8")
    return b.ljust(size, b"\0")


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
    """Overlay folders override loose files in Data Files, which override BSA contents; later BSAs override earlier ones."""

    def __init__(self, bsa_names=("Morrowind.bsa", "Tribunal.bsa", "Bloodmoon.bsa")):
        self.bsas = [BSA(DATA_FILES / n) for n in bsa_names if (DATA_FILES / n).exists()]

    def exists(self, name):
        name = name.lower().replace("/", "\\")
        return any((d / name).exists() for d in OVERLAYS) or (DATA_FILES / name).exists() or any(name in b.files for b in self.bsas)

    def read(self, name):
        name = name.lower().replace("/", "\\")
        for d in reversed(OVERLAYS):
            if (d / name).exists():
                return (d / name).read_bytes()
        loose = DATA_FILES / name
        if loose.exists():
            return loose.read_bytes()
        for b in reversed(self.bsas):
            if name in b.files:
                return b.read(name)
        raise FileNotFoundError(name)

    def names(self):
        out = set()
        for b in self.bsas:
            out.update(b.files)
        for d in OVERLAYS:
            out.update(str(f.relative_to(d)).lower().replace("/", "\\") for f in d.rglob("*") if f.is_file())
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
        return None if d is None else text(d)

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


def masters(path):
    """The MAST names in a record file's TES3 header."""
    for r in read_records(path, {"TES3"}):
        return [text(d) for t, d in r.subs if t == "MAST"]
    return []


def load_order():
    """[Morrowind.esm, plugins...] as paths, each plugin's masters checked: every one must be loaded before it,
    and none may be Tribunal.esm or Bloodmoon.esm (out of this project's scope)."""
    order = [DATA_FILES / MASTER]
    for name in PLUGINS:
        path = DATA_FILES / name
        if not path.is_file():
            raise SystemExit(f"MW3DS_PLUGINS: {name} is not in {DATA_FILES}")
        loaded = {p.name.lower() for p in order}
        for m in masters(path):
            if m.lower() in OUT_OF_SCOPE:
                raise SystemExit(f"MW3DS_PLUGINS: {name} needs {m}; only Morrowind.esm content is supported")
            if m.lower() not in loaded:
                raise SystemExit(f"MW3DS_PLUGINS: {name} needs {m}, which is not loaded before it")
        order.append(path)
    return order


def content_key():
    """What converted data depends on besides the converters: the plugins (by content) and the text encoding.
    Empty for plain Morrowind.esm in win1252, so caches made before plugins existed stay valid."""
    if not PLUGINS and ENCODING == "win1252":
        return ""
    h = hashlib.sha1(ENCODING.encode())
    for path in load_order()[1:]:
        h.update(path.name.lower().encode() + b"\0" + hashlib.sha1(path.read_bytes()).digest())
    return h.hexdigest()[:16]


def _i32(d, off=0):
    return struct.unpack_from("<i", d, off)[0]


def _record_key(r):
    """What a later file's record replaces: same type and ID (indices for skills and magic effects)."""
    t = r.tag
    if t in ("SKIL", "MGEF"):
        return t, _i32(r.get("INDX"))
    if t == "SCPT":
        return t, text(r.get("SCHD")[:32]).lower()
    if t == "PGRD":
        gx, gy = struct.unpack_from("<ii", r.get("DATA"))
        name = r.zstr("NAME") or ""
        return t, ((gx, gy) if (gx or gy or not name) else name.lower())
    return t, (r.id or "").lower()


def _cell_key(header):
    data = dict(header)["DATA"]
    if struct.unpack_from("<I", data)[0] & 1:
        return "int", text(dict(header).get("NAME", b"")).lower()
    return "ext", struct.unpack_from("<ii", data, 4)


def _split_cell(r, file_index, parents):
    """A CELL record as (header subrecords, deleted, [(ref key, subrecords, deleted, moved to)]). A reference's key is
    (file it comes from, number): the FRMR's top byte, when set, picks one of this file's masters (OpenMW's
    adjustRefNum); MVRF + CNDT before a reference move it to another exterior cell."""
    header, refs, cur, moved, deleted = [], [], None, None, False
    for tag, d in r.subs:
        if tag == "MVRF":
            moved = [d, None]
            cur = None
        elif tag == "CNDT" and moved is not None:
            moved[1] = struct.unpack_from("<ii", d)
        elif tag == "FRMR":
            num = struct.unpack("<I", d[:4])[0]
            local = num >> 24
            key = (parents[local - 1], num & 0xFFFFFF) if 0 < local <= len(parents) else (file_index, num)
            cur = [key, [(tag, d)], False, moved[1] if moved else None]
            refs.append(cur)
            moved = None
        elif cur is None:
            if tag == "DELE":
                deleted = True
            elif tag != "NAM0":
                header.append((tag, d))
        elif tag == "NAM0":
            continue                 # count of temporary references, between the two groups
        else:
            cur[1].append((tag, d))
            if tag == "DELE":
                cur[2] = True
    return header, deleted, refs


class _InfoChain:
    """A topic's responses in order, as OpenMW's InfoOrder builds it: an INFO goes after the one its PNAM names (first
    when it names none, last when that one is unknown); a later file's INFO with the same INAM and PNAM replaces it in
    place, with another PNAM it moves. Deleted ones keep their place until all files are read (later INFOs may follow
    them), then go."""

    def __init__(self):
        self.nodes = {}              # INAM -> [record, prev in chain, next in chain, deleted, its PNAM]
        self.head = self.tail = None

    def _unlink(self, k):
        n = self.nodes[k]
        if n[1] is None:
            self.head = n[2]
        else:
            self.nodes[n[1]][2] = n[2]
        if n[2] is None:
            self.tail = n[1]
        else:
            self.nodes[n[2]][1] = n[1]
        n[1] = n[2] = None

    def _link_before(self, k, before):
        n = self.nodes[k]
        after = self.tail if before is None else self.nodes[before][1]
        n[1], n[2] = after, before
        if after is None:
            self.head = k
        else:
            self.nodes[after][2] = k
        if before is None:
            self.tail = k
        else:
            self.nodes[before][1] = k

    def insert(self, r):
        k, prev = r.zstr("INAM") or "", r.zstr("PNAM") or ""
        deleted = r.get("DELE") is not None
        n = self.nodes.get(k)
        if n is not None and n[4] == prev:
            n[0], n[3] = r, deleted
            return
        if not prev:
            before = self.head
        elif prev in self.nodes:
            before = self.nodes[prev][2]
        else:
            before = None
        if n is None:
            self.nodes[k] = [r, None, None, deleted, prev]
            self._link_before(k, before)
            return
        n[0], n[3], n[4] = r, deleted, prev
        if before == k or (before is not None and before == n[2]) or (before is None and self.tail == k):
            return                   # already there (std::list::splice before itself or its next: no move)
        self._unlink(k)
        self._link_before(k, before)

    def records(self):
        k, out = self.head, []
        while k is not None:
            n = self.nodes[k]
            if not n[3]:
                out.append(n[0])
            k = n[2]
        return out


_merged = None


def _merge():
    """Every record of the load order, merged (see read_game). Kept for the rest of the run."""
    global _merged
    if _merged is not None:
        return _merged
    order = load_order()
    index = {p.name.lower(): i for i, p in enumerate(order)}
    recs = {}                        # (type, key) -> record, first file's order
    cells = {}                       # cell key -> [header {tag: data} in order, deleted, {ref key: [subs, deleted]}]
    moved = {}                       # ref key -> (target grid, subrecords, deleted)
    dials = {}                       # topic (lower) -> [DIAL record, _InfoChain]
    ltex_ids, ltex_paths = {}, {}    # (file, index) -> LTEX id; id -> its record (a later file's wins)
    for fi, path in enumerate(order):
        parents = [index[m.lower()] for m in masters(path)] if fi else []
        dial = None
        for r in read_records(path):
            t = r.tag
            if t == "TES3":
                continue
            if t == "DIAL":
                key = (r.id or "").lower()
                dial = dials.get(key)
                if r.get("DELE") is not None:
                    dials.pop(key, None)
                    recs.pop(("DIAL", key), None)
                    dial = None
                    continue
                if dial is None:
                    dial = dials[key] = [r, _InfoChain()]
                    recs[("DIAL", key)] = r
                else:
                    # The topic keeps the first file's letter case; its type is the later file's
                    dial[0] = Record("DIAL", r.flags, [("NAME", dial[0].get("NAME")), ("DATA", r.get("DATA"))])
                    recs[("DIAL", key)] = dial[0]
                continue
            if t == "INFO":
                if dial is not None:
                    dial[1].insert(r)
                continue
            if t == "CELL":
                header, deleted, refs = _split_cell(r, fi, parents)
                key = _cell_key(header)
                cell = cells.get(key)
                if cell is None:
                    cell = cells[key] = [{}, False, {}]
                    recs[("CELL", key)] = None
                for tag, d in header:
                    cell[0][tag] = d
                cell[1] = deleted
                for rkey, subs, rdel, target in refs:
                    if target is not None:
                        moved[rkey] = (target, subs, rdel)
                    else:
                        cell[2][rkey] = [subs, rdel]
                continue
            if t == "LTEX":
                lid = (r.id or "").lower()
                if r.get("DELE") is None:
                    ltex_ids[(fi, _i32(r.get("INTV")))] = lid
                    ltex_paths[lid] = r
                    recs.setdefault(("LTEX", lid), None)
                continue
            if t == "LAND":
                key = ("LAND", struct.unpack("<ii", r.get("INTV")[:8]))
                if r.get("DELE") is not None:
                    recs.pop(key, None)
                else:
                    recs[key] = (fi, r)
                continue
            key = _record_key(r)
            if r.get("DELE") is not None:
                recs.pop(key, None)
            else:
                recs[key] = r

    # Land textures: one index space, Morrowind.esm's indices kept, a plugin's new ones after them
    gindex = {}
    for (fi, i), lid in sorted(ltex_ids.items()):
        if fi == 0:
            gindex[lid] = i
    for lid in ltex_paths:
        if lid not in gindex:
            gindex[lid] = max(gindex.values(), default=-1) + 1

    out = []
    for key, r in recs.items():
        t = key[0]
        if t == "DIAL":
            dial = dials[key[1]]
            out.append(dial[0])
            out.extend(dial[1].records())
        elif t == "CELL":
            header, deleted, refs = cells[key[1]]
            if deleted:
                continue
            subs = list(header.items())
            for rkey, (rsubs, rdel) in refs.items():
                if not rdel and rkey not in moved:
                    subs.extend(rsubs)
            # References other cells' plugins moved into this one (OpenMW's leased references)
            if key[1][0] == "ext":
                for target, rsubs, rdel in moved.values():
                    if not rdel and tuple(target) == key[1][1]:
                        subs.extend(rsubs)
            out.append(Record("CELL", 0, subs))
        elif t == "LTEX":
            lr = ltex_paths[key[1]]
            subs = [(tag, struct.pack("<i", gindex[key[1]]) if tag == "INTV" else d) for tag, d in lr.subs]
            out.append(Record("LTEX", lr.flags, subs))
        elif t == "LAND":
            fi, lr = r
            out.append(_remap_land(lr, fi, ltex_ids, gindex) if fi else lr)
        else:
            out.append(r)
    # Exterior cells that only a moved reference names
    for target, rsubs, rdel in moved.values():
        if not rdel and ("ext", tuple(target)) not in cells:
            cells[("ext", tuple(target))] = None
            data = struct.pack("<Iii", 2, *target)
            out.append(Record("CELL", 0, [("NAME", b"\0"), ("DATA", data)] + list(rsubs)))
    _merged = out
    return out


def _remap_land(r, fi, ltex_ids, gindex):
    """A plugin's LAND: its VTEX values (land texture index + 1, 0 = default) name that plugin's own LTEX
    records (OpenMW looks them up by plugin and index); turned into the merged index space."""
    vtex = r.get("VTEX")
    if vtex is None:
        return r
    vals = struct.unpack(f"<{len(vtex) // 2}H", vtex)
    new = []
    for v in vals:
        lid = ltex_ids.get((fi, v - 1)) if v else None
        new.append(gindex[lid] + 1 if lid in gindex else 0)
    vtex = struct.pack(f"<{len(new)}H", *new)
    return Record("LAND", r.flags, [(t, vtex if t == "VTEX" else d) for t, d in r.subs])


def read_game(wanted=None):
    """The game's records: Morrowind.esm with the plugins of MW3DS_PLUGINS laid over it as OpenMW does. A later
    record of the same type and ID replaces the whole record and one with DELE removes it; cells merge their
    references by number (a plugin changes, deletes or adds some, MVRF moves one to another exterior cell);
    a topic's responses merge by INAM in the PNAM chain; LAND takes the later file's whole record."""
    if not PLUGINS:
        yield from read_records(DATA_FILES / MASTER, wanted)
        return
    for r in _merge():
        if not wanted or r.tag in wanted:
            yield r


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
            cur["id"] = text(d)
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
            cur["dest_cell"] = text(d)
        elif tag == "FLTV":
            cur["lock"] = struct.unpack("<i", d)[0]
        elif tag == "KNAM":
            cur["key"] = text(d)
        elif tag == "NAM9":
            cur["count"] = struct.unpack("<i", d)[0]
        elif tag == "XSOL":
            cur["soul"] = text(d).lower()     # a filled soul gem lying about
        elif tag == "XCHG":
            cur["charge"] = struct.unpack("<f", d)[0]                           # an enchanted item's charge
    return [r for r in refs if r["pos"] and not r["deleted"]]


def load_db(path=None):
    """One pass over the game's records (read_game; `path`: that one file as it is) collecting everything the level
    tools use."""
    db = {"objects": {}, "SCPT": {}, "GLOB": {}, "GMST": {}, "CLAS": {}, "RACE": {}, "BSGN": {},
          "SPEL": {}, "SOUN": {}, "SKIL": {}, "CELL": {}, "FACT": {}, "LAND": {}, "LTEX": {}, "REGN": {},
          "LEVC": {}, "LEVI": {}, "MGEF": {}, "PGRD": {}, "dialogue": []}
    dial = None
    skip = {"SNDG", "ENCH", "TES3"}
    for r in (read_records(path) if path else read_game()):
        t = r.tag
        if t in skip:
            continue
        if t == "DIAL":
            dial = {"name": r.id, "type": r.get("DATA")[0], "infos": []}
            db["dialogue"].append(dial)
        elif t == "INFO":
            dial["infos"].append(r)
        elif t == "SCPT":
            name = text(r.get("SCHD")[:32]).lower()
            db["SCPT"][name] = r.zstr("SCTX") or ""
        elif t == "GLOB":
            v = r.get("FLTV")
            db["GLOB"][r.id.lower()] = (r.zstr("FNAM"), struct.unpack("<f", v)[0] if v else 0.0)
        elif t == "GMST":
            if r.get("STRV") is not None:
                val = text(r.get("STRV"))
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
                    cur = text(d)
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
