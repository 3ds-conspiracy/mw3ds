"""Padding an actor's picture before it is shrunk for the console: each part of the sheet grown into the gaps round it.

Used by textures.py's encode. Kept out of textures.py: that file is part of the cell cache's key (convert_cell.py),
and how the gaps are filled changes no cell. Run on its own, it pads again the pictures of data converted before
(from the texture cache: same source, size and settings), without converting the rest:

    python tools/convert/texpad.py --data out/world
    python tools/convert/texpad.py --data out/data-tribunal-bloodmoon --tribunal --bloodmoon   (the mods' folders in
                                                                         MW3DS_OVERLAY_DIRS, as build_cia_mods.py sets)
    python tools/convert/texpad.py --data out/o3ds --max-tex 128         (an Old 3DS's: its creatures go to CREATURE_CAP)
"""
import argparse
import io
import json
import os
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


# A creature's or NPC's picture is a sheet of its parts side by side (fur, face, mouth, claws). Shrunk for the console, a
# texel takes in its neighbours, and a part close to another took in its colours: the mouth's red and black ran into the
# fur of a bear's back, a streak along it. Before it is shrunk, the texels no model of it reaches take the colour of the
# nearest one that does (each part grown into the gaps round it), so a part only ever takes in itself. Only pictures
# that only actors' libraries draw: the islands are theirs, and a world model's or a first-person piece's (some slide
# their pictures, a flipbook's frames) would lose what lies outside them
PAD_VERSION = 2
# Texels this see-through count as gaps too where every model draws the picture with its alpha (blended or tested:
# alpha tests start at 100). Shrinking takes them in all the same: their colour came into the edge of a fringe, the fur
# along a bear's back drawn dark there, a dashed line
SEE_THROUGH = 64
# The Old 3DS's 128-texel cap left a creature's sheet a blur (a bear's 512: fur, face, mouth and claws on one picture).
# Creatures' pictures get 256 there: one sheet a kind of creature, a few kinds in view, about 65 KB more each; people,
# their clothes and the world stay at the console's cap (spec/performance-tradeoffs.md)
CREATURE_CAP = 256


def _unpacked(path):
    data = path.read_bytes()
    return zlib.decompress(data[8:]) if data[:4] == b"MWZ1" else data


def _library_meshes(data):
    """(texture name, flags, uv array, index array) of each mesh in an 'MWL1' library (actors.py write_actor_library)"""
    out, o = [], 12
    if data[:4] != b"MWL1":
        return out
    for _ in range(struct.unpack_from("<I", data, 8)[0]):
        n = struct.unpack_from("<H", data, o)[0]
        name = data[o + 2:o + 2 + n].decode("latin-1")
        o += 2 + n
        flags, _, kind, more, _, nv, ni = struct.unpack_from("<BBBBiII", data, o)
        o += 16
        verts = np.frombuffer(data, dtype=[("pos", "<f4", 3), ("uv", "<f4", 2), ("c", "u1", 4)], count=nv, offset=o)
        o += nv * 24
        idx = np.frombuffer(data, dtype="<u2", count=ni, offset=o)
        o += ni * 2 + (2 if ni & 1 else 0)
        if kind == 1:
            pal = struct.unpack_from("<I", data, o)[0]
            o += 4 + pal * 52 + nv * 8
        if more & 1:
            nm = struct.unpack_from("<4fI", data, o)[4]
            o += 20
            for _ in range(nm):
                nk = struct.unpack_from("<I", data, o)[0]
                o += 4 + nk * 8 + nv * 12
        if more & 2:
            o += 4 + struct.unpack_from("<I", data, o)[0] * 8
        if more & 16:
            o += 4
        out.append((name, flags, verts["uv"].copy(), idx.copy()))
    return out


def actor_texture_islands(out_dir):
    """{texture name: [(uv, indices, flags), ...]} for the pictures actor libraries draw, with the islands of the cells' own
    batches that draw with them too (a bear's sheet on the pelts lying about): what pad_islands keeps; none that a
    first-person piece names or that slides (flipbooks, scrolls: their texels move)"""
    from textures import texture_out_name       # (textures.py imports this module)
    islands, other = {}, set()
    for p in (out_dir / "fp").glob("*.fpm"):
        data = _unpacked(p)
        if data[:4] == b"MWP1":
            for k in range(struct.unpack_from("<I", data, 4)[0]):
                other.add(texture_out_name("textures\\" + data[8 + 64 * k:72 + 64 * k].split(b"\0")[0].decode("latin-1")))
    for p in (out_dir / "cells" / "actors").rglob("*.aml"):
        try:
            meshes = _library_meshes(_unpacked(p))
        except (struct.error, ValueError):
            continue
        for name, flags, uv, idx in meshes:
            name = name.lower()
            if flags & 64:                     # (sliding: BATCH_SCROLL)
                other.add(name)
            islands.setdefault(name, []).append((uv, idx, flags))
    for p in (out_dir / "cells").rglob("*.cel"):
        data = _unpacked(p)
        if data[:4] != b"MWC1":
            continue
        # (its static batches, after its texture list: 84 bytes, then the vertices as 16 planes of a byte each, the
        # indices as 2 planes of differences; UV s16 x the batch's scale, convert_cell.write_batches)
        nt = struct.unpack_from("<I", data, 72)[0]
        names = [data[76 + 64 * k:140 + 64 * k].split(b"\0")[0].decode("latin-1").lower() for k in range(nt)]
        o = 76 + 64 * nt
        count = struct.unpack_from("<I", data, o)[0]
        o += 4
        for _ in range(count):
            tex = struct.unpack_from("<i", data, o)[0]
            flags = data[o + 4]
            nv, ni = struct.unpack_from("<II", data, o + 8)
            uvs = struct.unpack_from("<2f", data, o + 64)
            flip = data[o + 72]
            vbase, ibase = o + 84, o + 84 + nv * 16
            o = ibase + ni * 2 + (2 if ni & 1 else 0)
            name = names[tex] if 0 <= tex < nt else ""
            if name not in islands:
                continue
            if flags & 64 or flip:
                other.add(name)
                continue
            planes = np.frombuffer(data, np.uint8, nv * 16, vbase).reshape(16, nv)
            u = (planes[8].astype(np.uint16) | (planes[9].astype(np.uint16) << 8)).view(np.int16) * uvs[0]
            v = (planes[10].astype(np.uint16) | (planes[11].astype(np.uint16) << 8)).view(np.int16) * uvs[1]
            ip = np.frombuffer(data, np.uint8, ni * 2, ibase).reshape(2, ni)
            d = ip[0].astype(np.uint32) | (ip[1].astype(np.uint32) << 8)
            idx = (np.cumsum(d) & 0xFFFF).astype(np.int64)
            islands[name].append((np.stack([u, v], axis=1), idx, flags))
    return {k: v for k, v in islands.items() if k not in other and k}


def creature_textures(out_dir):
    """Names of the pictures only creatures' libraries draw (game_actors_*.json marks a creature and names its library):
    not one a person draws too (armour a skeleton and a guard share), which would raise people's"""
    import json
    names, people = set(), set()
    for f in sorted(out_dir.glob("game_actors_*.json")):
        for a in json.loads(_unpacked(f)).get("actors", []):
            lib = out_dir / a["lib"] if a.get("lib") else None
            if not lib or not lib.exists():
                continue
            try:
                drawn = {name.lower() for name, _, _, _ in _library_meshes(_unpacked(lib))}
            except (struct.error, ValueError):
                continue
            (names if a.get("creature") else people).update(drawn)
    return names - people


def pad_islands(img, meshes, grow):
    """The picture with each texel no triangle of `meshes` covers, or too see-through to be drawn, taken from the nearest
    one that is drawn, `grow` texels out (colour only: the alpha stays as it is)"""
    w, h = img.size
    mask_img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(mask_img)
    for uv, idx, _ in meshes:
        # (the library's v is flipped for the GPU: the picture's row is 1 - v)
        px = np.stack([uv[:, 0] * w, (1.0 - uv[:, 1]) * h], axis=1)
        for t in idx[: len(idx) // 3 * 3].reshape(-1, 3):
            tri = px[t]
            shift = np.floor(tri.min(axis=0) / [w, h]) * [w, h]
            tri = tri - shift
            for dx in (0, -w):
                for dy in (0, -h):
                    draw.polygon([tuple(v + [dx, dy]) for v in tri], fill=255)
    rgba = np.asarray(img.convert("RGBA")).astype(np.float32)
    mask = np.asarray(mask_img) > 0
    # (only when every model of it draws it with its alpha: one drawing it solid shows a see-through texel's colour, a
    # riekling's skin)
    if all(flags & 3 for _, _, flags in meshes):
        mask &= rgba[..., 3] >= SEE_THROUGH
    if mask.mean() > 0.97 or not mask.any():
        return img
    col = rgba[..., :3].copy()
    # (a texel's edge counts: one more round of the covered ones first)
    for _ in range(grow + 1):
        acc = np.zeros_like(col)
        cnt = np.zeros(mask.shape, np.float32)
        for sy, sx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            m = np.roll(mask, (sy, sx), axis=(0, 1))
            acc += np.roll(col, (sy, sx), axis=(0, 1)) * m[..., None]
            cnt += m
        new = ~mask & (cnt > 0)
        if not new.any():
            break
        col[new] = acc[new] / cnt[new][:, None]
        mask = mask | new
    rgba[..., :3] = col
    return Image.fromarray(rgba.clip(0, 255).astype(np.uint8), "RGBA")


def repad(out_dir, max_tex):
    """Encodes again each padded picture of a converted data folder whose padding is older than PAD_VERSION, and each
    creature's picture the console's cap (max_tex) held under CREATURE_CAP"""
    import textures
    from mwfiles import OVERLAYS, Archives, content_key
    cache = json.loads(textures.CACHE.read_text()) if textures.CACHE.exists() else {}
    overlay = ";".join(str(d) for d in OVERLAYS) + content_key()
    islands = actor_texture_islands(out_dir)
    creatures = creature_textures(out_dir)
    arch, tex3ds = Archives(), textures.tex3ds_path()
    done = other = 0
    for name in sorted(set(islands) | creatures):
        entry = cache.get(name)
        if not entry:
            continue
        # (held at the cap, it needed at least as much: what encode gives it now)
        limit = entry["limit"]
        if name in creatures and limit == max_tex < CREATURE_CAP:
            limit = min(CREATURE_CAP, max(entry.get("source_pot", [limit])))
        padded = PAD_VERSION if name in islands else entry.get("padded", 0)
        if entry.get("padded", 0) == padded and entry["limit"] == limit:
            continue
        if entry.get("overlay", "") != overlay:
            other += 1                 # (converted with other mods or add-ons than these settings: not this folder's)
            continue
        img = Image.open(io.BytesIO(arch.read(entry["src"]))).convert("RGBA")
        if name in islands:
            img = pad_islands(img, islands[name], 4 * max(1, img.size[0] // max(1, limit)) + 8)
        new = textures.encode_one(tex3ds, img, textures.tex_path(name), limit, entry["want_alpha"],
                                  entry.get("sheet", False), entry.get("mips", True), name in islands)
        entry.update(new)
        entry["limit"] = limit
        entry["padded"] = padded
        done += 1
    textures.CACHE.write_text(json.dumps(cache, indent=0, sort_keys=True))
    return done, other


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="the converted data folder (out/world, out/o3ds, ...)")
    ap.add_argument("--tribunal", action="store_true", help="it was converted with the Tribunal add-on")
    ap.add_argument("--bloodmoon", action="store_true", help="it was converted with the Bloodmoon add-on")
    ap.add_argument("--max-tex", type=int, default=256, help="the cap it was converted with (level.py --max-tex)")
    a = ap.parse_args()
    # (textures.py reads where the data and its cache are when it is imported; mwfiles reads the add-ons each time)
    os.environ["MW3DS_OUT"] = a.data
    if a.tribunal:
        os.environ["MW3DS_TRIBUNAL"] = "1"
    if a.bloodmoon:
        os.environ["MW3DS_BLOODMOON"] = "1"
    done, other = repad(Path(a.data), a.max_tex)
    print(f"texpad: {done} pictures encoded again in {a.data}")
    if other:
        sys.exit(f"texpad: {other} were converted with other mods or add-ons: run it with the settings of that "
                 f"conversion (MW3DS_OVERLAY_DIRS, --tribunal, --bloodmoon)")


if __name__ == "__main__":
    main()
