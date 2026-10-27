"""Pictures the original shows outside the world: loading-screen splashes (Data Files\\Splash, loose TGAs),
the level-up screen's art per class (textures\\levelup), and the pictures inside books (bookart\\).

Each becomes data/art/<kind>/<name>.t3x (RGB565, or RGBA4 with transparency), padded to powers of two;
game.json "art": {"splash": [{"file", "w", "h"}...], "levelup": {class: {...}}, "bookart": {name: {...}},
"book": {name: {...}}} with the picture's own size inside the padded texture. "book" is the reading screen's own
look: the open book (tx_menubook) and the scroll (scroll.dds), sized for the bottom screen, and their buttons
(tx_menubook_<take|close|next|prev>_<idle|over|pressed>). "menu": the main menu's picture (textures menu_morrowind.dds,
as OpenMW's main menu shows it) for the title screen, at a fixed name, art/menu_morrowind.t3x: the title screen runs
before game.json is read. Rain and ash particles' pictures go to art/weather/<name>.t3x, found there by name
(weather_art; Bloodmoon's snow and blizzard too when its archive is there).

Usage (the weather pictures alone, into data converted already):
    MW3DS_OUT=out/tb python tools/convert/artassets.py --weather
"""
import io
import re
from pathlib import Path

from PIL import Image

from mwfiles import bloodmoon, game_path
from textures import OUT, encode_image

SPLASH_SIZE = (400, 240)          # the top screen
# The reading screen on the bottom screen: OpenMW's book window shows tx_menubook's middle (its 512 x 256 stretched
# to 728 wide, 584 of that in view: x 50 to 461) at 584 x 398, the scroll window scroll.dds at 512 x 372; both
# scaled to the screen's 320 width
BOOK_CROP = (50, 0, 461, 256)
BOOK_SIZE = (320, 218)
SCROLL_SIZE = (320, 232)
BOOK_BUTTONS = [f"{b}_{s}" for b in ("take", "close", "next", "prev") for s in ("idle", "over", "pressed")]


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


def rain_sheet(drop, size=(128, 256), drops=90, seed=7):
    """The far rain's picture (Session::drawPrecipitation's rings): raindrop.nif's drop, its middle column stretched to
    8 to 20 texels, scattered over a sheet that repeats both ways, white with the drop's alpha doubled."""
    import random
    w, h = size
    core = [drop.getpixel((drop.size[0] // 2, y))[3] for y in range(drop.size[1])]
    alpha = [[0] * w for _ in range(h)]
    rnd = random.Random(seed)
    for _ in range(drops):
        x, y0, length = rnd.randrange(w), rnd.randrange(h), rnd.randint(8, 20)
        strength = rnd.uniform(0.6, 1.0)
        for k in range(length):
            a = core[min(len(core) - 1, k * len(core) // length)] * 2.0 * strength
            row = alpha[(y0 + k) % h]
            row[x] = min(255, max(row[x], round(a)))
    img = Image.new("RGBA", size, (255, 255, 255, 0))
    img.putdata([(255, 255, 255, alpha[y][x]) for y in range(h) for x in range(w)])
    return img


def splash_ring(size=32):
    """A drop's ripple (Session::drawPrecipitation): a soft white ring, brightest at 0.75 of the radius, clear inside."""
    import math
    img = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    c = (size - 1) / 2.0
    img.putdata([(255, 255, 255, round(255 * max(0.0, 1.0 - abs(math.hypot(x - c, y - c) / c - 0.75) / 0.25)))
                 for y in range(size) for x in range(size)])
    return img


def snow_sheet(sheet, size=128, flakes=48, seed=5):
    """Snow's picture (Session::drawPrecipitation): snow.nif's Tx_BM_Snowflakes_01 is a 256-texel sheet with a few dozen
    specks on it, each particle 75 units across; drawn so on the console most of a sheet is see-through screen the GPU
    blends for nothing. Here its specks are cut out (each brightest texel's 5 x 5) and four dozen of them scattered over a
    smaller sheet, the alpha doubled (the top screen's 240 lines)"""
    import random
    w, h = sheet.size
    alpha = sheet.split()[3].load()
    specks = []
    for y in range(2, h - 2):
        for x in range(2, w - 2):
            a = alpha[x, y]
            if a > 60 and all(a >= alpha[x + i, y + j] for i in (-1, 0, 1) for j in (-1, 0, 1)):
                specks.append([[alpha[x + i, y + j] for i in range(-2, 3)] for j in range(-2, 3)])
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    px = out.load()
    rnd = random.Random(seed)
    for k in range(min(flakes, len(specks)) if specks else 0):
        s = specks[rnd.randrange(len(specks))]
        x0, y0 = rnd.randrange(3, size - 3), rnd.randrange(3, size - 3)
        for j in range(5):
            for i in range(5):
                a = min(255, round(s[j][i] * 2.0))
                if a > px[x0 + i - 2, y0 + j - 2][3]:
                    px[x0 + i - 2, y0 + j - 2] = (255, 255, 255, a)
    return out


def weather_art(arch):
    """Rain, ash and snow in the world (Session::drawPrecipitation): raindrop.nif's drop, ashcloud.nif's flake and the
    dust clouds of ashcloud.nif, blightcloud.nif and Bloodmoon's blizzard.nif, at art/weather/<name>.t3x. Their alpha
    twice the original's: on the top screen's 240 lines OpenMW's faint drops and flakes were all but gone, and its
    clouds lost in the fog. Snow's flakes from snow.nif's sheet (snow_sheet)"""
    for key, path, gain in (("rain", r"textures\tx_raindrop_01.dds", 2.0), ("ash_flake", r"textures\tx_ash_flake.dds", 2.0),
                            ("ash_cloud", r"textures\tx_ash_cloud.dds", 2.0), ("blight_cloud", r"textures\tx_blight_cloud.dds", 2.0),
                            ("blizzard_cloud", r"textures\tx_bm_blizzard_01.dds", 2.0)):
        # (Bloodmoon's only with the add-on: its archive is read for every conversion, and without the flag the output
        # stays as it was)
        if arch.exists(path) and (bloodmoon() or "_bm_" not in path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGBA")
            if gain != 1.0:
                r, g, b, a = img.split()
                img = Image.merge("RGBA", (r, g, b, a.point(lambda x: min(255, round(x * gain)))))
            encode(img, OUT / "art" / "weather" / f"{key}.t3x", True)
    if bloodmoon() and arch.exists(r"textures\tx_bm_snowflakes_01.dds"):
        sheet = Image.open(io.BytesIO(arch.read(r"textures\tx_bm_snowflakes_01.dds"))).convert("RGBA")
        encode(snow_sheet(sheet), OUT / "art" / "weather" / "snow_flake.t3x", True)
    if arch.exists(r"textures\tx_raindrop_01.dds"):
        drop = Image.open(io.BytesIO(arch.read(r"textures\tx_raindrop_01.dds"))).convert("RGBA")
        encode(rain_sheet(drop), OUT / "art" / "weather" / "rain_sheet.t3x", True)
        encode(splash_ring(), OUT / "art" / "weather" / "splash.t3x", True)


def build_art(arch, data_files, birthsigns=()):
    art = {"splash": [], "levelup": {}, "bookart": {}, "hud": {}, "birthsign": {}}
    # The birthsign menu's constellations (the sign record's texture), by sign id: shown on the top screen
    for sign in birthsigns:
        base = re.sub(r"\.[^.]+$", "", (sign.get("texture") or "").lower().replace("/", "\\"))
        if base and not base.startswith("textures\\"):
            base = "textures\\" + base           # (the record names it from textures\: Birthsigns\Tx_birth_mage.tga)
        for ext in (".dds", ".tga", ".bmp"):
            try:
                img = Image.open(io.BytesIO(arch.read(base + ext))).convert("RGB")
            except (FileNotFoundError, KeyError, OSError, ValueError):
                continue
            if img.size[0] > 256:
                img = img.resize((256, max(1, round(img.size[1] * 256 / img.size[0]))), Image.LANCZOS)
            stem = re.sub(r"[^a-z0-9_]", "_", sign["id"].lower())
            w, h = encode(img, OUT / "art" / "birthsign" / f"{stem}.t3x", False)
            art["birthsign"][sign["id"].lower()] = {"file": f"art/birthsign/{stem}.t3x", "w": w, "h": h}
            break
    # HUD pieces: the sneak eye (shown while sneaking unseen)
    # ... and Bloodmoon's werewolf: the view's overlay (the fur at the edges, drawn over the whole top screen while the
    # player is a werewolf, OpenMW's setWerewolfOverlay) and the claw that stands for hand to hand on the HUD
    hud = [("sneak", r"icons\k\stealth_sneak.dds")]
    if bloodmoon():
        hud += [("werewolf_overlay", r"textures\werewolfoverlay.dds"), ("werewolf_hand", r"icons\k\tx_werewolf_hand.dds")]
    for key, path in hud:
        if arch.exists(path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGBA")
            if max(img.size) > 256:
                img = img.resize((min(256, img.size[0]), min(256, img.size[1])), Image.LANCZOS)
            w, h = encode(img, OUT / "art" / "hud" / f"{key}.t3x", True)
            art["hud"][key] = {"file": f"art/hud/{key}.t3x", "w": w, "h": h}
    # The reading screen: the open book, the scroll, their buttons
    art["book"] = {}
    for key, path, crop, size in (("book", r"textures\tx_menubook.dds", BOOK_CROP, BOOK_SIZE),
                                  ("scroll", r"textures\scroll.dds", None, SCROLL_SIZE)):
        if arch.exists(path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGBA")
            if crop:
                img = img.crop(crop)
            # (on black, as the screen behind it: RGB565 keeps the parchment's shading, RGBA4 bands it)
            img = Image.alpha_composite(Image.new("RGBA", size, (0, 0, 0, 255)), img.resize(size, Image.LANCZOS))
            w, h = encode(img.convert("RGB"), OUT / "art" / "book" / f"{key}.t3x", False)
            art["book"][key] = {"file": f"art/book/{key}.t3x", "w": w, "h": h}
    for key in BOOK_BUTTONS:
        path = rf"textures\tx_menubook_{key}.dds"
        if arch.exists(path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGBA")
            w, h = encode(img, OUT / "art" / "book" / f"{key}.t3x", True)
            art["book"][key] = {"file": f"art/book/{key}.t3x", "w": w, "h": h}
    # Enchanted items' shimmer: the 32 frames of textures\magicitem\caust (32 x 32), drawn over the item in the
    # colour of its enchantment's first effect; the engine finds them at art/magicitem/caustNN.t3x
    for i in range(32):
        path = rf"textures\magicitem\caust{i:02d}.dds"
        if arch.exists(path):
            img = Image.open(io.BytesIO(arch.read(path))).convert("RGB")
            encode(img, OUT / "art" / "magicitem" / f"caust{i:02d}.t3x", False)
    weather_art(arch)
    # The water surface's frames, as Morrowind and OpenMW animate water (Morrowind.ini [Water]: SurfaceTexture, frames
    # SurfaceFrameCount at SurfaceFPS, SurfaceTextureSize texels): data/art/water/waterNN.t3x, ETC1. The ripples change
    # shape; one picture slid over the water looked still on the console
    ini = {}
    try:
        text = game_path(Path(data_files).parent, "Morrowind.ini").read_text(encoding="latin-1", errors="replace")
        part = re.search(r"^\[Water\](.*?)(?=^\[|\Z)", text, re.M | re.S)
        for line in (part.group(1).splitlines() if part else []):
            k, eq, v = line.partition("=")
            if eq and not k.strip().startswith(";"):
                ini[k.strip().lower()] = v.strip()
    except OSError:
        pass
    stem = ini.get("surfacetexture", "water")
    count = int(float(ini.get("surfaceframecount", 32) or 32))
    size = pow2(int(float(ini.get("surfacetexturesize", 128) or 128)))
    frames = 0
    for i in range(min(count, 64)):
        try:
            img = Image.open(io.BytesIO(arch.read(f"textures\\water\\{stem}{i:02d}.dds"))).convert("RGB")
        except (KeyError, OSError):
            break
        out = OUT / "art" / "water" / f"water{i:02d}.t3x"
        out.parent.mkdir(parents=True, exist_ok=True)
        encode_image(img.resize((size, size), Image.LANCZOS), out, "etc1")
        frames += 1
    if frames:
        art["water"] = {"frames": frames, "fps": float(ini.get("surfacefps", 12) or 12)}
    # The main menu's picture (OpenMW's MainMenu background), on the title screen's top screen, stretched as Morrowind
    # stretched it: the 2:1 picture filled a 4:3 screen, 1.5 times taller than it is, and the logo movie before it was
    # drawn to match (fitted frame against picture: no other shape lines up its emblem and lettering). Its middle at
    # the screen's 5:3 across and 5:3 / 1.5 up and down, the logo whole (it spans the middle 80% across)
    try:
        img = Image.open(io.BytesIO(arch.read("textures\\menu_morrowind.dds"))).convert("RGB")
        w, h = img.size
        cw = min(w, round(h * SPLASH_SIZE[0] / SPLASH_SIZE[1]))
        ch = min(h, round(cw * SPLASH_SIZE[1] / SPLASH_SIZE[0] / 1.5))
        img = img.crop(((w - cw) // 2, (h - ch) // 2, (w - cw) // 2 + cw, (h - ch) // 2 + ch)).resize(SPLASH_SIZE, Image.LANCZOS)
        w, h = encode(img, OUT / "art" / "menu_morrowind.t3x", False)
        art["menu"] = {"file": "art/menu_morrowind.t3x", "w": w, "h": h}
    except (KeyError, OSError):
        pass
    # Loading screens
    splash_dir = game_path(data_files, "Splash")
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


if __name__ == "__main__":
    import argparse
    from mwfiles import Archives
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weather", action="store_true", help="write the weather pictures (art/weather) into MW3DS_OUT")
    ap.add_argument("--bloodmoon", action="store_true", help="with Bloodmoon's snow and blizzard (or MW3DS_BLOODMOON=1)")
    args = ap.parse_args()
    if args.bloodmoon:
        import os
        os.environ["MW3DS_BLOODMOON"] = "1"
    if args.weather:
        weather_art(Archives())
        print(f"wrote {OUT / 'art' / 'weather'}")
