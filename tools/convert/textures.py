"""Texture conversion sized for the 3DS top screen.

The top screen is 400x240 with a 60 degree vertical field of view, so at a close
viewing distance of CLOSE_DISTANCE units one screen pixel covers PIXEL_WORLD units.
Each use of a texture tells us how many world units one texture repeat spans
(texel density); a texture never needs more texels than the pixels it can fill
at that distance. Sizes are the next power of two above that, between MIN_SIZE and
the level's maximum, and never above the source. Mipmaps cover farther views.

Output: out/data/textures/<name>.t3x (ETC1, or ETC1A4 when alpha matters), mipmapped.
Sizes chosen per run are cached in out/texture_cache.json so unchanged textures
are not re-encoded, and .t3x files no level cell uses any more are removed.
"""
import errno
import hashlib
import io
import json
import zlib
import math
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

import envfile  # noqa: F401  (loads .env)
from mwfiles import OVERLAYS, content_key

# Converted data goes here (MW3DS_OUT overrides it, e.g. out/world for a whole-island trial run)
OUT = Path(os.environ.get("MW3DS_OUT") or Path(__file__).resolve().parents[2] / "out" / "data")
# (one per converted data folder: out/data and out/world share the first, as they always have; another folder, say the
# Old 3DS build's smaller textures, keeps its own so the two don't re-encode each other's)
CACHE = Path(__file__).resolve().parents[2] / "out" / ("texture_cache.json" if OUT.name in ("data", "world")
                                                       else f"texture_cache_{OUT.name}.json")

SCREEN_HEIGHT = 240
FOV_Y = math.radians(60.0)
CLOSE_DISTANCE = 96.0          # arm's length: items on a table, a face in dialogue
PIXEL_WORLD = CLOSE_DISTANCE * 2 * math.tan(FOV_Y / 2) / SCREEN_HEIGHT
MIN_SIZE = 32
# The 3DS's largest texture side
MAX_SIDE = 1024


def is_sheet(path):
    """A sheet of several pictures side by side (Project Atlas's: textures/atl/..., *_atlas*): the size cap is per
    picture, so it applies to the sheet's short side."""
    p = path.lower().replace("\\", "/")
    return "/atl/" in p or "atlas" in p.rsplit("/", 1)[-1]


def tex3ds_path():
    for cand in (os.environ.get("TEX3DS"), os.path.join(os.environ.get("MW3DS_MSYS2_DIR", ""), "opt", "devkitpro", "tools", "bin", "tex3ds.exe"),
                 "/opt/devkitpro/tools/bin/tex3ds", shutil.which("tex3ds")):
        if cand and Path(cand).exists():
            return cand
    raise SystemExit("tex3ds not found (set TEX3DS to its path)")


def shard_dir(name):
    """Subfolder (of 64) a data file lives in: the 3DS's SD file system scans a whole folder to
    create or open a file, so thousands in one folder crawl (source/components/files/datapath.hpp computes the same)."""
    return "%02x" % (zlib.crc32(name.encode("latin-1")) & 63)


def tex_path(name):
    """out/data/tex/<shard>/<name>"""
    return OUT / "tex" / shard_dir(name) / name


def migrate_flat(old_dir, new_dir, pattern):
    """Moves files of an older, unsharded layout into their shard folders (no re-encoding)."""
    if not old_dir.exists():
        return
    for p in old_dir.glob(pattern):
        dest = new_dir / shard_dir(p.name) / p.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            p.unlink()
        else:
            p.rename(dest)
    try:
        old_dir.rmdir()
    except OSError:
        pass


def texture_out_name(path):
    stem = path[len("textures\\"):] if path.startswith("textures\\") else path
    stem = re.sub(r"\.[^.]+$", "", stem)
    return re.sub(r"[^a-z0-9_]", "_", stem.lower()) + ".t3x"


def world_per_repeat(pos, uv, tris):
    """World units covered by one UV unit, from the more stretched part of a shape
    (area-weighted 75th percentile over its triangles); 0 when UVs are degenerate."""
    t = np.asarray(tris).reshape(-1, 3)
    if not len(t):
        return 0.0
    p, q = pos[t], uv[t]
    wa = 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1)
    ua = 0.5 * np.abs((q[:, 1, 0] - q[:, 0, 0]) * (q[:, 2, 1] - q[:, 0, 1])
                      - (q[:, 2, 0] - q[:, 0, 0]) * (q[:, 1, 1] - q[:, 0, 1]))
    ok = (ua > 1e-10) & (wa > 1e-4)
    if not ok.any():
        return 0.0
    ratio, weight = np.sqrt(wa[ok] / ua[ok]), wa[ok]
    order = np.argsort(ratio)
    cum = np.cumsum(weight[order])
    return float(ratio[order][np.searchsorted(cum, 0.75 * cum[-1])])


def texels_needed(world_units):
    """Texels one repeat needs so it is never magnified at CLOSE_DISTANCE."""
    return world_units / PIXEL_WORLD


def pow2_ceil(v):
    return 1 << max(0, math.ceil(math.log2(max(v, 1.0))))


class TextureSet:
    """Collects every texture the level's cells use, then encodes each once at the
    largest size any use needs."""

    def __init__(self, arch, max_size=256, force=False, mips=True):
        self.arch, self.max_size, self.force, self.mips = arch, max_size, force, mips
        self.req = {}                  # source path -> {"px": texels, "alpha": bool}
        self.whitened = {}             # output name -> {"src", "px", "alpha", "amount"}: a picture drawn whiter
        self.made = set()              # output names encoded elsewhere (particles.py's flipbook sheets): kept
        self.generated = {}            # output name -> PIL picture made by the converter (a world spray's loop sheet)
        self.log = None                # a list: requests are also recorded there (a cell's, for the cell cache)

    def request(self, path, texels, alpha):
        """Returns the output name the cell file refers to."""
        if self.log is not None:
            self.log.append((path, float(texels), bool(alpha)))
        r = self.req.setdefault(path, {"px": 0.0, "alpha": False})
        r["px"] = max(r["px"], texels)
        r["alpha"] = r["alpha"] or bool(alpha)
        return texture_out_name(path)

    def request_whitened(self, path, texels, alpha, amount):
        """A whiter copy of the picture (whiten_image), its own output name ('<name>_w'): a spell visual's thin strip
        whose white core is lost on the 3DS's small screen (firstperson.VFX_BOLD)."""
        name = texture_out_name(path)[:-4] + "_w.t3x"
        r = self.whitened.setdefault(name, {"src": path, "px": 0.0, "alpha": False, "amount": amount})
        r["px"] = max(r["px"], texels)
        r["alpha"] = r["alpha"] or bool(alpha)
        return name

    def request_shape(self, s, alpha):
        return self.request(s["tex"], texels_needed(world_per_repeat(s["pos"], s["uv"], s["tris"])), alpha)

    def limit(self, texels):
        return max(MIN_SIZE, min(self.max_size, pow2_ceil(texels)))

    def encode(self):
        cache = json.loads(CACHE.read_text()) if CACHE.exists() and not self.force else {}
        tex3ds = None
        migrate_flat(OUT / "textures", OUT / "tex", "*.t3x")
        (OUT / "tex").mkdir(parents=True, exist_ok=True)
        stats = {"count": 0, "encoded": 0, "bytes": 0, "full_bytes": 0, "failed": [], "broken": []}
        # the mod folders laid over Data Files (mwfiles.OVERLAYS): with others, the same path can be another picture
        # (and the plugins: they come with their own loose textures, which may replace others)
        overlay = ";".join(str(d) for d in OVERLAYS) + content_key()
        wanted = set()
        items = sorted(self.req.items())
        print(f"textures: checking {len(items)} (new or resized ones are encoded with tex3ds; the first run takes a while)",
              flush=True)
        for i, (path, r) in enumerate(items):
            if i and i % 50 == 0:
                print(f"  textures {i}/{len(items)}, {stats['encoded']} encoded", flush=True)
            name = texture_out_name(path)
            if name in self.generated:
                continue                       # (made by the converter: encoded below)
            wanted.add(name)
            limit = self.limit(r["px"])
            try:
                img = None
                entry = cache.get(name)
                sheet = is_sheet(path)
                if not (entry and entry.get("src") == path and entry.get("limit") == limit
                        and entry.get("want_alpha") == r["alpha"] and entry.get("overlay", "") == overlay
                        and entry.get("sheet", False) == sheet and entry.get("mips", True) == self.mips
                        and tex_path(name).exists()):
                    img = Image.open(io.BytesIO(self.arch.read(path)))
                    img.load()
                    tex3ds = tex3ds or tex3ds_path()
                    tex_path(name).parent.mkdir(parents=True, exist_ok=True)
                    entry = encode_one(tex3ds, img.convert("RGBA"), tex_path(name), limit, r["alpha"], sheet, self.mips)
                    entry.update({"src": path, "limit": limit, "want_alpha": r["alpha"], "overlay": overlay,
                                  "sheet": sheet})
                    cache[name] = entry
                    stats["encoded"] += 1
            except Exception as e:           # missing or unreadable: the cell draws it untextured
                stats["failed"].append(f"{path}: {e}")
                # (a picture that is there but couldn't be written: tex3ds failing, a full disk; the build stops)
                if isinstance(e, subprocess.CalledProcessError) or getattr(e, "errno", None) == errno.ENOSPC:
                    stats["broken"].append(f"{path}: {e}")
                continue
            stats["count"] += 1
            stats["bytes"] += etc_bytes(entry["size"], entry["alpha"])
            stats["full_bytes"] += etc_bytes(entry["source_pot"], entry["alpha"], cap=self.max_size)
        for name, r in sorted(self.whitened.items()):
            wanted.add(name)
            limit = self.limit(r["px"])
            entry = cache.get(name)
            try:
                if not (entry and entry.get("src") == r["src"] and entry.get("limit") == limit
                        and entry.get("whiten") == r["amount"] and entry.get("mips", True) == self.mips
                        and entry.get("overlay", "") == overlay and tex_path(name).exists()):
                    img = Image.open(io.BytesIO(self.arch.read(r["src"])))
                    img.load()
                    tex3ds = tex3ds or tex3ds_path()
                    tex_path(name).parent.mkdir(parents=True, exist_ok=True)
                    entry = encode_one(tex3ds, whiten_image(img.convert("RGBA"), r["amount"]), tex_path(name), limit,
                                       r["alpha"], mips=self.mips)
                    entry.update({"src": r["src"], "limit": limit, "want_alpha": r["alpha"], "whiten": r["amount"],
                                  "overlay": overlay})
                    cache[name] = entry
                    stats["encoded"] += 1
            except Exception as e:
                stats["failed"].append(f"{r['src']} (whitened): {e}")
                if isinstance(e, subprocess.CalledProcessError) or getattr(e, "errno", None) == errno.ENOSPC:
                    stats["broken"].append(f"{r['src']} (whitened): {e}")
                continue
            stats["count"] += 1
            stats["bytes"] += etc_bytes(entry["size"], entry["alpha"])
        wanted |= self.made
        # (pictures the converter made, at their own size: a world spray's looping flipbook sheet)
        for name, img in sorted(self.generated.items()):
            wanted.add(name)
            digest = hashlib.sha1(img.tobytes()).hexdigest()
            entry = cache.get(name)
            if not (entry and entry.get("digest") == digest and entry.get("mips", True) == self.mips
                    and tex_path(name).exists()):
                tex3ds = tex3ds or tex3ds_path()
                tex_path(name).parent.mkdir(parents=True, exist_ok=True)
                entry = encode_one(tex3ds, img, tex_path(name), max(img.size), True, mips=self.mips)
                entry.update({"src": "generated", "digest": digest})
                cache[name] = entry
                stats["encoded"] += 1
            stats["count"] += 1
            stats["bytes"] += etc_bytes(entry["size"], entry["alpha"])
        for p in (OUT / "tex").rglob("*.t3x"):
            if p.name not in wanted:
                p.unlink()
                cache.pop(p.name, None)
        CACHE.write_text(json.dumps(cache, indent=0, sort_keys=True))
        stats["sizes"] = {name: etc_bytes(e["size"], e["alpha"]) for name, e in cache.items() if name in wanted}
        return stats


def encode_image(img, out, fmt="etc1"):
    """Encodes a PIL image (power-of-two sides) as-is, without mipmaps (map pictures)."""
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(tmp) / "in.png"
        img.save(png)
        subprocess.run([tex3ds_path(), "-f", fmt, "-o", str(out), str(png)], check=True, capture_output=True)


def etc_bytes(size, alpha, cap=None):
    w, h = size
    while cap and max(w, h) > cap:
        w, h = max(8, w // 2), max(8, h // 2)
    return int(w * h * (1.0 if alpha else 0.5) * 4 / 3)    # 4 or 8 bpp, plus mipmaps


def whiten_image(img, amount):
    """Colours moved that share of the way to white (alpha kept): a purple-edged white core reads white."""
    a = np.asarray(img, dtype=float)
    a[..., :3] = 255.0 - (255.0 - a[..., :3]) * (1.0 - amount)
    return Image.fromarray(a.clip(0, 255).astype(np.uint8), "RGBA")


def encode_one(tex3ds, img, out, limit, want_alpha, sheet=False, mips=True):
    """A sheet (is_sheet) is capped on its short side, the long side up to MAX_SIDE: taken on the long side, an Old
    3DS's 128 left each of the eight pictures of a 256 x 2048 sheet 16 texels across."""
    w, h = img.size
    pot = lambda v: 1 << max(3, round(math.log2(max(v, 1))))
    sw, sh = pot(w), pot(h)
    tw, th = sw, sh
    while (min(tw, th) if sheet else max(tw, th)) > limit or max(tw, th) > MAX_SIDE:
        tw, th = max(8, tw // 2), max(8, th // 2)
    if (tw, th) != (w, h):
        img = img.resize((tw, th), Image.LANCZOS)
    has_alpha = want_alpha and img.getextrema()[3][0] < 255
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(tmp) / "in.png"
        img.save(png)
        # (an Old 3DS draws every world texture at full size, never its smaller copies: its data carries none, a third less)
        cmd = [tex3ds, "-f", "etc1a4" if has_alpha else "etc1", *(["-m", "triangle"] if mips else []), "-o", str(out), str(png)]
        # tex3ds can fail under memory pressure and leave a cut-short file behind, which the game then fails to read
        # (it shipped in a build as a 4 KB stub): once more, and never a partial file on disk
        for attempt in (1, 2):
            try:
                subprocess.run(cmd, check=True, capture_output=True)
                break
            except subprocess.CalledProcessError:
                Path(out).unlink(missing_ok=True)
                if attempt == 2:
                    raise
    return {"size": [tw, th], "source_pot": [sw, sh], "alpha": has_alpha, "mips": mips}
