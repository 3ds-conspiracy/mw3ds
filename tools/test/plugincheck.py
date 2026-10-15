"""Checks the converter's plugin merge (tools/convert/mwfiles.py read_game) rule by rule, against the plugins in
your own Data Files (Bethesda's official ones work: adamantiumarmor.esp, AreaEffectArrows.esp, bcsounds.esp,
EBQ_Artifact.esp, entertainers.esp, LeFemmArmor.esp, master_index.esp, Siege at Firemoth.esp). The rules are in
spec/plugins-and-translations.md.

  python tools/test/plugincheck.py                        every .esp in Data Files whose masters are loaded
  python tools/test/plugincheck.py --plugins "a.esp;b.esp"

Prints one line per check and "RESULT: PASS" or "RESULT: FAIL (n failed)"; exit status 1 on a failure.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "convert"))
import mwfiles  # noqa: E402
from mwfiles import DATA_FILES, MASTER, cell_refs, read_records  # noqa: E402

failures = []


def check(ok, what):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures.append(what)


def cell_key(r):
    d = r.get("DATA")
    if struct.unpack_from("<I", d)[0] & 1:
        return r.id.lower()
    return struct.unpack_from("<ii", d, 4)


def refnum_key(fi, sub, masters):
    num = struct.unpack("<I", sub[:4])[0]
    local = num >> 24
    return (masters[local - 1], num & 0xFFFFFF) if 0 < local <= len(masters) else (fi, num)


def frmr_groups(r):
    """[(FRMR bytes, ref subrecords)] of a CELL record, MVRF / NAM0 aside."""
    out = []
    for t, d in r.subs:
        if t == "FRMR":
            out.append((d, [(t, d)]))
        elif out and t not in ("NAM0", "MVRF", "CNDT"):
            out[-1][1].append((t, d))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugins", help="';'-separated plugin names (default: every .esp needing only Morrowind.esm)")
    args = ap.parse_args()
    if args.plugins:
        plugins = [p for p in args.plugins.split(";") if p]
    else:
        plugins = sorted(p.name for p in DATA_FILES.glob("*.esp")
                         if all(m.lower() == MASTER.lower() for m in mwfiles.masters(p)))
    if not plugins:
        sys.exit("plugincheck: no plugins to check in " + str(DATA_FILES))
    print("plugins:", "; ".join(plugins))

    # The master alone: the merge gives back exactly what the file holds
    mwfiles.PLUGINS, mwfiles._merged = [], None
    raw = [r for r in read_records(DATA_FILES / MASTER) if r.tag != "TES3"]
    alone = mwfiles._merge()
    same = len(raw) == len(alone) and sorted(map(repr, ((r.tag, r.id) for r in raw))) == \
        sorted(map(repr, ((r.tag, r.id) for r in alone)))
    check(same, f"{MASTER} alone: the merge holds its {len(raw)} records")

    mwfiles.PLUGINS, mwfiles._merged = plugins, None
    order = mwfiles.load_order()
    merged = mwfiles._merge()
    db = mwfiles.load_db()
    index = {p.name.lower(): i for i, p in enumerate(order)}

    # 1. A later record of the same type and ID replaces the whole record; DELE removes it
    last, deleted = {}, set()
    for fi, path in enumerate(order):
        for r in read_records(path):
            if r.tag in ("TES3", "CELL", "DIAL", "INFO", "LAND", "LTEX", "PGRD"):
                continue
            key = mwfiles._record_key(r)
            if r.get("DELE") is not None:
                last.pop(key, None)
                deleted.add(key)
            else:
                last[key] = (fi, r)
                deleted.discard(key)
    got = {mwfiles._record_key(r): r for r in merged
           if r.tag not in ("CELL", "DIAL", "INFO", "LAND", "LTEX", "PGRD")}
    overridden = [k for k, (fi, r) in last.items() if fi > 0]
    bad = [k for k, (fi, r) in last.items() if got.get(k) is None or got[k].subs != r.subs]
    check(not bad, f"records: the last file's version wins ({len(overridden)} from plugins){' ' + str(bad[:3]) if bad else ''}")
    check(not any(k in got for k in deleted), f"records: DELE removes ({len(deleted)})")

    # 2. Cells: references by (file, number); a plugin's own replaces in place, DELE removes, new ones join
    plugin_cells = {}
    for fi, path in enumerate(order[1:], 1):
        masters = [index[m.lower()] for m in mwfiles.masters(path)]
        for r in read_records(path, {"CELL"}):
            for frmr, subs in frmr_groups(r):
                plugin_cells.setdefault(cell_key(r), []).append((refnum_key(fi, frmr, masters), subs, fi))
    merged_cells = {cell_key(r): r for r in merged if r.tag == "CELL"}
    master_cells = {cell_key(r): r for r in read_records(DATA_FILES / MASTER, {"CELL"})}
    changed = deleted_refs = added = 0
    bad_cells = []
    for ck, refs in plugin_cells.items():
        m = merged_cells.get(ck)
        if m is None:
            bad_cells.append(ck)
            continue
        groups = frmr_groups(m)
        keys = [refnum_key(0, f, []) for f, _ in groups]       # merged keys: (0, n) master / (fi, n) own ...
        final = {}
        for key, subs, fi in refs:
            final[key] = subs
        for key, subs in final.items():
            dele = any(t == "DELE" for t, _ in subs)
            present = [g for g in groups if g[1] == subs]
            if dele:
                deleted_refs += 1
                if present:
                    bad_cells.append((ck, "deleted ref kept"))
            else:
                if key[0] == 0:
                    changed += 1
                else:
                    added += 1
                if not present:
                    bad_cells.append((ck, "ref missing", key))
        del keys
        # As many references as the master's, plus the new ones, less the deleted ones (none twice)
        base = master_cells.get(ck)
        base_refs = {struct.unpack("<I", f[:4])[0] for f, s in frmr_groups(base) if not any(t == "DELE" for t, _ in s)} \
            if base else set()
        want = len(base_refs) + sum(1 for k, s in final.items() if k[0] != 0 and not any(t == "DELE" for t, _ in s)) \
            - sum(1 for k, s in final.items() if k[0] == 0 and k[1] in base_refs and any(t == "DELE" for t, _ in s))
        if len(cell_refs(m)) != want:
            bad_cells.append((ck, f"{len(cell_refs(m))} references, {want} wanted"))
    check(not bad_cells, f"cells: plugin references in place ({changed} changed, {added} new, {deleted_refs} deleted)"
          f"{' ' + str(bad_cells[:3]) if bad_cells else ''}")
    # A master reference a plugin changed keeps its place among the cell's references
    moved_place = 0
    for ck, refs in plugin_cells.items():
        base = next((r for r in read_records(DATA_FILES / MASTER, {"CELL"}) if cell_key(r) == ck), None) \
            if any(k[0] == 0 for k, _, _ in refs) else None
        if base is None:
            continue
        order_base = [struct.unpack("<I", f[:4])[0] for f, _ in frmr_groups(base)]
        order_merged = [struct.unpack("<I", f[:4])[0] & 0xFFFFFF for f, _ in frmr_groups(merged_cells[ck])]
        kept = [n for n in order_merged if n in order_base]
        expect = [n for n in order_base if n in kept]
        moved_place += kept != expect
    check(moved_place == 0, "cells: master references keep their order")

    # 3. Dialogue: each plugin INFO sits after the one its PNAM names (first when empty), deleted ones are gone
    misplaced, placed, gone = [], 0, 0
    chains = {d["name"].lower(): [i.zstr("INAM") for i in d["infos"]] for d in db["dialogue"]}
    final_info = {}
    for path in order[1:]:
        topic = None
        for r in read_records(path, {"DIAL", "INFO"}):
            if r.tag == "DIAL":
                topic = r.id.lower()
            elif topic:
                final_info[(topic, r.zstr("INAM"))] = r
    for (topic, inam), r in final_info.items():
        chain = chains.get(topic, [])
        if r.get("DELE") is not None:
            gone += 1
            if inam in chain:
                misplaced.append((topic, inam, "deleted kept"))
            continue
        if inam not in chain:
            misplaced.append((topic, inam, "missing"))
            continue
        i, prev = chain.index(inam), r.zstr("PNAM") or ""
        if (chain[i - 1] if i else "") != prev and prev in chain:
            misplaced.append((topic, inam, "not after its PNAM"))
        placed += 1
    check(not misplaced, f"dialogue: {placed} plugin responses after their PNAM, {gone} deleted"
          f"{' ' + str(misplaced[:3]) if misplaced else ''}")

    # 4. Land: a plugin's LAND names its own LTEX records by index; the merged one names the same textures
    bad_land, lands = [], 0
    for fi, path in enumerate(order[1:], 1):
        own = {struct.unpack("<i", r.get("INTV"))[0]: r.zstr("DATA") for r in read_records(path, {"LTEX"})}
        for r in read_records(path, {"LAND"}):
            vtex = r.get("VTEX")
            grid = struct.unpack("<ii", r.get("INTV")[:8])
            if vtex is None or db["LAND"].get(grid) is not r and db["LAND"].get(grid) is None:
                continue
            lands += 1
            want = [own.get(v - 1) if v else None for v in struct.unpack(f"<{len(vtex) // 2}H", vtex)]
            mv = db["LAND"][grid].get("VTEX")
            have = [db["LTEX"].get(v - 1) if v else None for v in struct.unpack(f"<{len(mv) // 2}H", mv)]
            if want != have:
                bad_land.append(grid)
    check(not bad_land, f"land: {lands} plugin LAND records keep their textures{' ' + str(bad_land[:3]) if bad_land else ''}")

    # 5. Load order: a plugin that isn't there, or needs Tribunal / Bloodmoon, is refused
    for names, why in ((["no such plugin.esp"], "missing plugin refused"),):
        mwfiles.PLUGINS = names
        try:
            mwfiles.load_order()
            check(False, why)
        except SystemExit:
            check(True, why)

    # 6. Text: the code page setting, file names for letters outside ASCII
    check(mwfiles.text(b"wulfharth\x92s cups\0") == "wulfharth’s cups" or mwfiles.ENCODING != "win1252",
          "text: win1252 0x92 is a right single quote")
    a, b = mwfiles.safe_name("Балмора"), mwfiles.safe_name("Вивек")
    check(a != b and mwfiles.safe_name("Seyda Neen, Arrille's Tradehouse") == "seyda_neen__arrille_s_tradehouse",
          "file names: ASCII names as before, others told apart by a hash")

    print(f"RESULT: {'PASS' if not failures else f'FAIL ({len(failures)} failed)'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    main()
