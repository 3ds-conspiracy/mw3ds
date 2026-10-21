"""Checks how the converter finds loose files (tools/convert/mwfiles.py Archives): overlays over Data Files over the
BSAs, by the game's names (any case, backslashes) on every file system. Linux and macOS tell case apart and take a
backslash for part of a name, so a plain path finds none of them there (issue #62).

  python tools/test/loosecheck.py                          a made-up Data Files in a temp folder, names in odd case
  python tools/test/loosecheck.py --real --write <json>    Windows: your install and the mods build's overlays, the
                                                           lookup against Windows' own (as before), every name the
                                                           records give and every loose file; writes what it found
  python3 tools/test/loosecheck.py --mirror <json>         Linux: the same names as empty files in a temp folder
                                                           (a case-telling file system), the same answers wanted

Prints one line per check (--real and --mirror: the mismatches, up to 20) and "RESULT: PASS" or "RESULT: FAIL (n
failed)"; exit status 1 on a failure.
"""
import argparse
import json
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "convert"))
# The mods build's overlays, in its order (tools/build/build_cia.py OVERLAYS)
MOD_OVERLAYS = ["mods/morrowind-optimization-patch/00 Core", "mods/project-atlas/00 Core",
                "mods/project-atlas/01 Textures - Vanilla"]

failures = []


def check(ok, what):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures.append(what)


def use_install(mw_dir, overlays, made_up=False):
    """Import mwfiles for this install and these overlays (it reads them when imported). A made-up install has no
    plugins and the default encoding; a real one keeps your settings (.env)."""
    os.environ["MW3DS_MORROWIND_DIR"] = str(mw_dir)
    os.environ["MW3DS_OVERLAY_DIRS"] = ";".join(str(d) for d in overlays)
    if made_up:
        os.environ["MW3DS_PLUGINS"] = ""
        os.environ["MW3DS_ENCODING"] = "win1252"
    import mwfiles
    return mwfiles


def write_bsa(path, files):
    """A TES3 BSA holding {name: bytes} (hashes left zero: the converter does not read them)."""
    names = list(files)
    blob = b"".join(files[n] for n in names)
    name_bytes = b"".join(n.encode("latin-1") + b"\0" for n in names)
    name_offsets, at = [], 0
    for n in names:
        name_offsets.append(at)
        at += len(n) + 1
    sizes_offsets, at = [], 0
    for n in names:
        sizes_offsets += [len(files[n]), at]
        at += len(files[n])
    hash_offset = len(names) * 12 + len(name_bytes)
    with open(path, "wb") as f:
        f.write(struct.pack("<3I", 0x100, hash_offset, len(names)))
        f.write(struct.pack(f"<{len(names) * 2}I", *sizes_offsets))
        f.write(struct.pack(f"<{len(names)}I", *name_offsets))
        f.write(name_bytes)
        f.write(b"\0" * (len(names) * 8))
        f.write(blob)


def made_up():
    """Mixed-case Data Files, an overlay and a BSA in a temp folder, looked up by the names records use."""
    tmp = Path(tempfile.mkdtemp(prefix="mw3ds-loose-"))
    data = tmp / "Morrowind" / "Data Files"
    over = tmp / "Mod" / "00 Core"
    files = {
        data / "Sound" / "Fx" / "Menu Click.wav": b"loose click",
        data / "Sound" / "Vo" / "d" / "m" / "Atk_DM001.mp3": b"loose voice",
        data / "Music" / "Explore" / "mx_explore_1.mp3": b"loose music",
        data / "Meshes" / "Were_Morph.NIF": b"loose morph",
        data / "Meshes" / "Only_Loose.nif": b"loose only",
        over / "meshes" / "were_morph.nif": b"overlay morph",
        over / "Textures" / "TX_Overlay.dds": b"overlay texture",
    }
    for p, b in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b)
    write_bsa(data / "Morrowind.bsa", {"meshes\\only_loose.nif": b"bsa only loose", "textures\\tx_bsa.dds": b"bsa texture",
                                       "textures\\tx_overlay.dds": b"bsa overlay texture"})
    mwfiles = use_install(data.parent, [over], True)
    arch = mwfiles.Archives()

    def reads(name, want):
        try:
            got = arch.read(name)
        except FileNotFoundError:
            got = None
        check(got == want, f"read {name!r}: {want!r} (got {got!r})")

    for name, want in (("Sound\\Fx\\menu click.wav", b"loose click"), ("sound\\fx\\MENU CLICK.WAV", b"loose click"),
                       ("Sound/Fx/menu click.wav", b"loose click"), ("Sound\\vo\\d\\m\\Atk_DM001.mp3", b"loose voice"),
                       ("Music/Explore/mx_explore_1.mp3", b"loose music"), ("Sound\\\\Fx\\.\\menu click.wav", b"loose click"),
                       ("meshes\\were_morph.nif", b"overlay morph"),          # the overlay over Data Files
                       ("meshes\\only_loose.nif", b"loose only"),             # Data Files over the BSA
                       ("textures\\tx_overlay.dds", b"overlay texture"),      # the overlay over the BSA
                       ("Textures\\TX_BSA.dds", b"bsa texture"),              # the BSA when nothing loose has it
                       ("sound\\fx\\no such.wav", None),
                       ("sound\\vo\\..\\fx\\menu click.wav", b"loose click"),    # ".." takes back a part, as on Windows
                       ("sound\\fx\\menu click.wav.", b"loose click"),         # a trailing dot dropped, as on Windows
                       ("music\\explore\\mx_explore_1.mp3 ", b"loose music")):    # and a trailing space
        reads(name, want)
    check(arch.exists("SOUND\\FX\\Menu Click.wav"), "exists: a loose file in another case")
    check(arch.exists("textures\\tx_bsa.dds"), "exists: a BSA file")
    # (a folder: Windows' plain path finds it, as it always did; the index holds files only. No conversion asks for one)
    windows = os.sep == "\\"
    check(arch.exists("sound\\fx") == windows, f"exists: a folder only on Windows ({windows})")
    check(not arch.exists("sound\\fx\\no such.wav"), "exists: not a missing file")
    names = arch.names()
    check("textures\\tx_overlay.dds" in names and "meshes\\were_morph.nif" in names,
          "names: the overlay's files by game name")
    check("textures\\tx_bsa.dds" in names, "names: the BSA's files")


def query_names(mwfiles, roots):
    """The names a conversion can ask for: every file name in the records under the folder it lives in (and a .wav
    as .mp3, as convert_sound tries), and every loose file as written, in capitals and with forward slashes."""
    out = set()
    folders = {"nif": ["meshes\\"], "kf": ["meshes\\"], "dds": ["textures\\", "icons\\", "bookart\\", ""],
               "tga": ["textures\\", "icons\\", "bookart\\", ""], "bmp": ["textures\\", "icons\\", "bookart\\", ""],
               "wav": ["sound\\"], "mp3": ["sound\\", "music\\"]}
    for path in mwfiles.load_order():
        for r in mwfiles.read_records(path):
            for _, d in r.subs:
                if not 4 < len(d) < 260 or not d.rstrip(b"\0").isascii():
                    continue
                s = mwfiles.text(d)
                m = re.search(r"\.([a-z0-9]{2,3})$", s.lower())
                if not m or m.group(1) not in folders:
                    continue
                for f in folders[m.group(1)]:
                    out.add(f + s)
                    if m.group(1) == "wav":
                        out.add(f + s[:-4] + ".mp3")
    for _, files in roots:
        for rel in files:
            out.update((rel, rel.upper(), rel.replace("\\", "/")))
    return sorted(out)


def old_source(mwfiles, overlays, name):
    """Where the lookup before #62 found a name (Windows' own path lookup): (root index, file) or None."""
    name = name.lower().replace("/", "\\")
    for i in reversed(range(len(overlays))):
        if (overlays[i] / name).is_file():
            return i, overlays[i] / name
    if (mwfiles.DATA_FILES / name).is_file():
        return len(overlays), mwfiles.DATA_FILES / name
    return None


def new_source(mwfiles, arch, name):
    """Where Archives finds a name loose: (root index, file) or None (the root: the overlay or Data Files it is under;
    a later overlay first, as the lookup takes it)."""
    path = arch.loose_path(name)
    if path is None:
        return None
    real = os.path.realpath(path)
    dirs = list(mwfiles.OVERLAYS) + [mwfiles.DATA_FILES]
    for i in reversed(range(len(dirs))):
        root = os.path.realpath(dirs[i])
        if os.path.commonpath([real, root]) == root:
            return i, path
    return None


def answer(found, roots_dirs):
    """A found file as (root index, its real path under the root in lower case: the name on disk, not the asked one),
    for comparing across machines."""
    if found is None:
        return None
    i, path = found
    return [i, os.path.relpath(os.path.realpath(path), os.path.realpath(roots_dirs[i])).replace(os.sep, "\\").lower()]


def real(out_json):
    from envfile import require
    mw_dir = require("MW3DS_MORROWIND_DIR", "the folder of your Morrowind install")
    mwfiles = use_install(mw_dir, [ROOT / d for d in MOD_OVERLAYS])
    overlays = mwfiles.OVERLAYS
    dirs = overlays + [mwfiles.DATA_FILES]
    roots = []
    for d in dirs:
        files = []
        for top, _, names in os.walk(d):
            files += [os.path.relpath(os.path.join(top, n), d).replace(os.sep, "\\") for n in names]
        roots.append((str(d), sorted(files)))
    arch = mwfiles.Archives()
    queries = query_names(mwfiles, roots)
    wanted, bad = {}, []
    for q in queries:
        old, new = answer(old_source(mwfiles, overlays, q), dirs), answer(new_source(mwfiles, arch, q), dirs)
        if old != new:
            bad.append(f"{q!r}: Windows finds {old}, the index {new}")
        bsa = any(q.lower().replace("/", "\\") in b.files for b in arch.bsas)
        if arch.exists(q) != (old is not None or bsa):
            bad.append(f"{q!r}: exists {arch.exists(q)}, Windows {old is not None or bsa}")
        wanted[q] = old
    for line in bad[:20]:
        print(f"FAIL {line}")
    found = sum(1 for v in wanted.values() if v)
    check(not bad, f"{len(queries)} names ({found} found loose) looked up as Windows does: {len(bad)} differ")
    if out_json:
        Path(out_json).write_text(json.dumps({"roots": [files for _, files in roots], "wanted": wanted}),
                                  encoding="utf-8")
        print(f"  wrote {out_json}")


def mirror(in_json):
    """The --real run's files as empty files on this machine's file system, the same names looked up."""
    got = json.loads(Path(in_json).read_text(encoding="utf-8"))
    tmp = Path(tempfile.mkdtemp(prefix="mw3ds-mirror-"))
    *overlay_files, data_files = got["roots"]
    overlays = [tmp / f"overlay{i}" for i in range(len(overlay_files))]
    data = tmp / "Morrowind" / "Data Files"
    for root, files in zip(overlays + [data], overlay_files + [data_files]):
        for rel in files:
            if rel.lower().endswith(".bsa"):
                continue                     # (the archives aren't loose files: an empty one is no archive)
            p = root.joinpath(*rel.split("\\"))
            p.parent.mkdir(parents=True, exist_ok=True)
            p.touch()
    probe = tmp / "Probe"
    probe.touch()
    check(not (tmp / "probe").exists(), f"{tmp}: tells case apart (else this proves nothing)")
    mwfiles = use_install(data.parent, overlays, True)
    arch = mwfiles.Archives()
    dirs = overlays + [data]
    bad = []
    for q, want in got["wanted"].items():
        if q.lower().rstrip(". ").endswith(".bsa"):
            continue
        new = answer(new_source(mwfiles, arch, q), dirs)
        if new != want:
            bad.append(f"{q!r}: Windows found {want}, here {new}")
    for line in bad[:20]:
        print(f"FAIL {line}")
    found = sum(1 for v in got["wanted"].values() if v)
    check(not bad, f"{len(got['wanted'])} names ({found} found loose on Windows) found the same here: {len(bad)} differ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--real", action="store_true", help="check against your install (Windows)")
    ap.add_argument("--write", help="with --real: write the names and answers to this JSON file")
    ap.add_argument("--mirror", help="a --real run's JSON: look its names up in empty files here")
    args = ap.parse_args()
    if args.mirror:
        mirror(args.mirror)
    elif args.real:
        real(args.write)
    else:
        made_up()
    print("RESULT: PASS" if not failures else f"RESULT: FAIL ({len(failures)} failed)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
