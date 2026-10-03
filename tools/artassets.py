"""Pictures the original shows outside the world: loading-screen splashes (Data Files\\Splash, loose TGAs),
the level-up screen's art per class (textures\\levelup), and the pictures inside books (bookart\\).

Each becomes data/art/<kind>/<name>.t3x (RGB565, or RGBA4 with transparency), padded to powers of two;
game.json "art": {"splash": [{"file", "w", "h"}...], "levelup": {class: {...}}, "bookart": {name: {...}}}
with the picture's own size inside the padded texture.
"""
import io
import re
from pathlib import Path

from PIL import Image

from textures import OUT, encode_image

SPLASH_SIZE = (400, 240)          # the top screen


def pow2(v):
    p = 8
    while p < v:
        p *= 2
    return p


def encode(img, out_path, alpha):
    """Pads to powers of two (top-left), writes the texture; returns (w, h) of the picture."""
    w, h = img.size
    canvas = Image.new("RGBA" if alpha else "RGB", (pow2(w), pow2(h)), (0, 0, 0, 0) if alpha else (0, 0, 0))
    canvas.paste(img, (0, 0))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    encode_image(canvas, out_path, "rgba4" if alpha else "rgb565")
    return w, h


def build_art(arch, data_files):
    art = {"splash": [], "levelup": {}, "bookart": {}, "hud": {}}
    # HUD pieces: the sneak eye (shown while sneaking unseen)
    for key, path in (("sneak", r"icons\k\stealth_sneak.dds"),):
        if arch.exists(path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGBA")
            w, h = encode(img, OUT / "art" / "hud" / f"{key}.t3x", True)
            art["hud"][key] = {"file": f"art/hud/{key}.t3x", "w": w, "h": h}
    # Loading screens
    splash_dir = Path(data_files) / "Splash"
    for p in sorted(splash_dir.glob("*.tga")) if splash_dir.exists() else []:
        try:
            img = Image.open(p).convert("RGB").resize(SPLASH_SIZE, Image.LANCZOS)
        except Exception:
            continue
        name = re.sub(r"[^a-z0-9_]", "_", p.stem.lower())
        w, h = encode(img, OUT / "art" / "splash" / f"{name}.t3x", False)
        art["splash"].append({"file": f"art/splash/{name}.t3x", "w": w, "h": h})
    # Level-up art, one per class, and book pictures
    for name in sorted(arch.names()):
        low = name.lower().replace("/", "\\")
        kind = "levelup" if low.startswith("textures\\levelup\\") else "bookart" if low.startswith("bookart\\") else None
        if kind is None or not low.endswith((".dds", ".tga", ".bmp")):
            continue
        try:
            img = Image.open(io.BytesIO(arch.read(name)))
        except Exception:
            continue
        alpha = img.mode in ("RGBA", "LA") or "transparency" in img.info
        img = img.convert("RGBA" if alpha else "RGB")
        # small enough for the bottom screen / a book page, and the 1024 texture limit
        scale = min(1.0, 300.0 / img.size[0], 200.0 / img.size[1]) if kind == "bookart" else min(1.0, 256.0 / img.size[0])
        if scale < 1.0:
            img = img.resize((max(1, int(img.size[0] * scale)), max(1, int(img.size[1] * scale))), Image.LANCZOS)
        stem = re.sub(r"[^a-z0-9_]", "_", low.rsplit("\\", 1)[-1].rsplit(".", 1)[0])
        w, h = encode(img, OUT / "art" / kind / f"{stem}.t3x", alpha)
        # books name their pictures by path (BOOKART\\X.dds), level-up art by class
        key = low.rsplit("\\", 1)[-1].rsplit(".", 1)[0] if kind == "levelup" else low
        art[kind][key] = {"file": f"art/{kind}/{stem}.t3x", "w": w, "h": h}
    return art
