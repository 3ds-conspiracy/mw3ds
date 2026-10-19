"""Morrowind's own look for the menus: item icons, the window frame textures, the Magic Cards font
and the [Font Color] colours of Morrowind.ini. Everything is optional: whatever is missing, the 3DS
draws plainly instead.

Writes into out/data/ui/:
  icons_<n>.t3x   item icons, 32 x 32, 64 to a 256 x 256 page (RGBA4); objects get "ic": icon number
  atlas.t3x       window frame pieces (menu_thin_border_*, menu_button_frame_*, ...) packed (RGBA4)
  font.t3x        Magic Cards glyphs (A8)
and returns the game.json "ui" entry: {"icons", "atlas", "font", "colors"}.
"""
import io
import re
import struct

import numpy as np
from PIL import Image

from mwfiles import CODEC
from textures import OUT, encode_image

ICON = 32
DEFAULT_ICON = "default icon.tga"
ITEM_TYPES = {"MISC", "BOOK", "ALCH", "INGR", "WEAP", "ARMO", "CLOT", "LIGH", "APPA", "LOCK", "PROB", "REPA"}
ICON_PAGE = 256
UI_DIR = OUT / "ui"

# Window pieces: 9-slice frames (corners, edges) and single images
FRAME_PARTS = ["top_left_corner", "top", "top_right_corner", "left", "right",
               "bottom_left_corner", "bottom", "bottom_right_corner"]
FRAMES = ["menu_thin_border", "menu_thick_border", "menu_button_frame", "menu_head_block"]
SINGLES = ["menu_head_block_middle", "menu_icon_equip", "menu_icon_magic", "menu_icon_magic_equip",
           "menu_icon_barter", "compass", "door_icon"]

FONT = "magic_cards_regular"
FNT_HEADER = 296                 # float size, int 1, int 1, char name[284]
FNT_GLYPH = 56                   # 14 floats per character
FNT_SIZE = FNT_HEADER + 256 * FNT_GLYPH

# Which character each of a font's 256 slots holds (as OpenMW's FontLoader reads them): the win1252 releases' fonts
# (English, German, French ...) are in code page 437's order, not win1252's (a umlaut at 0x84, sharp s at 0xE1); the
# win1250 ones (Polish ...) in win1250 with these slots holding other letters (0: none); the win1251 ones (Russian)
# in win1251
FONT_CODEC = {"cp1252": "cp437"}
WIN1250_SLOTS = {0x80: 0xC6, 0x81: 0x9C, 0x82: 0xE6, 0x83: 0xB3, 0x84: 0xF1, 0x85: 0xB9, 0x86: 0xBF, 0x87: 0x9F,
                 0x88: 0xEA, 0x89: 0xEA, 0x8A: 0, 0x8B: 0, 0x8C: 0x8F, 0x8D: 0xAF, 0x8E: 0xA5, 0x8F: 0x8C, 0x90: 0xCA,
                 0x93: 0xA3, 0x94: 0xF6, 0x95: 0xF3, 0x96: 0xAF, 0x97: 0x8F, 0x99: 0xD3, 0x9A: 0xD1, 0x9C: 0, 0xA0: 0xB9,
                 0xA1: 0xAF, 0xA2: 0xF3, 0xA3: 0xBF, 0xA4: 0, 0xE1: 0x8C, 0xE3: 0, 0xF5: 0}
# Characters the fonts lack, drawn with one they have, by its slot (Morrowind's own stand-ins, as OpenMW's FontLoader
# lists them): curly quotes as straight ones, a low double quote as a comma, accented capitals as plain ones ...
FONT_STAND_INS = {
    34: "\u201c\u201d\u00a8", 37: "\u2030", 39: "\u2018\u2019\u00b4", 43: "\u2020", 44: "\u201a\u201e\u00b8",
    45: "\u2013\u2014\u00ad", 46: "\u2026", 49: "\u00b9", 51: "\u00b3", 60: "\u2039", 62: "\u203a",
    65: "\u00c0\u00c1\u00c2\u00c3", 68: "\u00d0", 69: "\u00c8\u00ca\u00cb\u0401", 73: "\u00cc\u00cd\u00ce\u00cf",
    79: "\u0152\u00d2\u00d3\u00d4\u00d5\u00d8", 83: "\u0160", 84: "\u2122", 85: "\u00d9\u00da\u00db",
    89: "\u0178\u00dd", 90: "\u017d", 94: "\u02c6", 95: "\u00af", 97: "\u00e3", 99: "\u00a9", 100: "\u00f0",
    111: "\u0153\u00f5\u00f8", 114: "\u00ae", 115: "\u0161", 120: "\u00d7", 121: "\u00fd", 122: "\u017e",
    126: "\u02dc", 137: "\u0451", 216: "\u2021", 221: "\u00a6",
}


def slot_char(i):
    """The character font slot i holds in this release's fonts ("" none)."""
    if CODEC == "cp1250":
        b = WIN1250_SLOTS.get(i, i)
        return bytes([b]).decode("cp1250", errors="ignore") if b else ""
    return bytes([i]).decode(FONT_CODEC.get(CODEC, CODEC), errors="ignore")

# Morrowind.ini [Font Color] defaults
COLOR_DEFAULTS = {
    "color_normal": "202,165,96", "color_normal_over": "223,201,159", "color_normal_pressed": "243,237,221",
    "color_active": "96,112,202", "color_disabled": "179,168,135", "color_link": "112,126,207",
    "color_link_over": "143,155,218", "color_link_pressed": "175,184,228", "color_header": "223,201,159",
    "color_notify": "223,201,159", "color_background": "0,0,0", "color_focus": "80,80,80",
    "color_health": "200,60,30", "color_magic": "53,69,159", "color_fatigue": "0,150,60",
    "color_count": "223,201,159", "color_positive": "223,201,159", "color_negative": "200,60,30",
}


def pow2(v):
    return max(8, 1 << (max(1, int(v)) - 1).bit_length())


def read_image(arch, path):
    """An image from the archives by its record path (.tga / .bmp in the records are .dds in the BSAs)."""
    base = re.sub(r"\.[^.]+$", "", path.lower().replace("/", "\\"))
    for ext in (".dds", ".tga", ".bmp"):
        try:
            return Image.open(io.BytesIO(arch.read(base + ext))).convert("RGBA")
        except (FileNotFoundError, KeyError, OSError, ValueError):
            continue
    return None


def build_icons(arch, objects, extra=()):
    """Item icons packed into pages; sets each object's "ic" (icon number) when its icon exists.
    objects: game.json "objects" (a dict by id, or a list)."""
    objects = list(objects.values()) if isinstance(objects, dict) else objects
    extra = list(extra)        # magic effects: their icons share the pages, "ic" set on each
    paths = sorted({o["icon"].lower().replace("/", "\\") for o in objects + extra if o.get("icon")} | {DEFAULT_ICON})
    index, images, missing = {}, [], []
    for p in paths:
        img = read_image(arch, "icons\\" + p)
        if img is None:
            missing.append(p)
            continue
        if img.size != (ICON, ICON):
            img = img.resize((ICON, ICON), Image.LANCZOS)
        index[p] = len(images)
        images.append(img)
    per_row = ICON_PAGE // ICON
    per_page = per_row * per_row
    UI_DIR.mkdir(parents=True, exist_ok=True)
    for old in UI_DIR.glob("icons_*.t3x"):
        old.unlink()
    pages = []
    for first in range(0, len(images), per_page):
        chunk = images[first:first + per_page]
        rows = (len(chunk) + per_row - 1) // per_row
        page = Image.new("RGBA", (ICON_PAGE, pow2(rows * ICON)), (0, 0, 0, 0))
        for k, img in enumerate(chunk):
            page.paste(img, ((k % per_row) * ICON, (k // per_row) * ICON))
        name = f"ui/icons_{len(pages)}.t3x"
        encode_image(page, OUT / name, "rgba4")
        pages.append({"file": name, "height": page.size[1]})
    # Items without a usable icon get Morrowind's own stand-in
    for o in objects:
        ic = index.get((o.get("icon") or "").lower().replace("/", "\\"), index.get(DEFAULT_ICON))
        if ic is not None and o.get("type") in ITEM_TYPES:
            o["ic"] = ic
    for e in extra:
        ic = index.get((e.get("icon") or "").lower().replace("/", "\\"))
        if ic is not None:
            e["ic"] = ic
    return {"size": ICON, "page": ICON_PAGE, "pages": pages}, len(images), missing


def pack(images, width):
    """Shelf packing, tallest first: {name: (x, y)} and the height used."""
    x = y = shelf = 0
    spots = {}
    for name, img in sorted(images.items(), key=lambda kv: -kv[1].size[1]):
        w, h = img.size
        if x + w > width:
            x, y, shelf = 0, y + shelf, 0
        spots[name] = (x, y)
        x += w + 1
        shelf = max(shelf, h + 1)
    return spots, y + shelf


def build_atlas(arch):
    images = {}
    for f in FRAMES:
        for part in FRAME_PARTS:
            img = read_image(arch, f"textures\\{f}_{part}.dds")
            if img is not None:
                images[f"{f}_{part}"] = img
    for s in SINGLES:
        img = read_image(arch, f"textures\\{s}.dds")
        if img is not None:
            images[s] = img
    if not images:
        return None
    # Long edge strips only need a stretch of their pattern
    for name, img in list(images.items()):
        w, h = img.size
        if w > 128 or h > 128:
            images[name] = img.resize((min(w, 128), min(h, 128)), Image.LANCZOS)
    width = 256
    spots, height = pack(images, width)
    if height > 256:
        width = 512
        spots, height = pack(images, width)
    atlas = Image.new("RGBA", (width, pow2(height)), (0, 0, 0, 0))
    for name, (x, y) in spots.items():
        atlas.paste(images[name], (x, y))
    UI_DIR.mkdir(parents=True, exist_ok=True)
    encode_image(atlas, OUT / "ui/atlas.t3x", "rgba4")
    return {"file": "ui/atlas.t3x", "width": atlas.size[0], "height": atlas.size[1],
            "pieces": {n: [x, y, images[n].size[0], images[n].size[1]] for n, (x, y) in spots.items()}}


def parse_font(fnt, tex):
    """Morrowind .fnt + .tex -> (A8 image, glyphs {codepoint: [x, y, w, h, advance, bearing x, bearing y]},
    line height), or None when the files don't look like a Morrowind font."""
    if len(fnt) < FNT_SIZE or len(tex) < 8:
        return None
    size, one_a, one_b = struct.unpack_from("<fii", fnt, 0)
    tw, th = struct.unpack_from("<ii", tex, 0)
    if one_a != 1 or one_b != 1 or not (0 < tw <= 2048 and 0 < th <= 2048) or len(tex) < 8 + tw * th * 4:
        return None
    rgba = np.frombuffer(tex, np.uint8, tw * th * 4, 8).reshape(th, tw, 4)
    ink = rgba[..., 3] if rgba[..., 3].min() < 255 else rgba[..., :3].max(axis=2)
    rects = []
    for i in range(256):
        g = struct.unpack_from("<14f", fnt, FNT_HEADER + i * FNT_GLYPH)
        # unknown, top left, top right, bottom left, bottom right, width, height, kerning left / right, ascent
        # (as OpenMW reads them: advance = width + right kerning, bearing = left kerning, size - ascent)
        _, tlx, tly, trx, _try, _blx, bly, _brx, _bry, width, height, kern_l, kern_r, ascent = g
        x1, y1 = tlx * tw, tly * th
        rects.append((i, x1, y1, trx * tw - x1, bly * th - y1, width + kern_r, kern_l, ascent))
    # Rows are stored top-down as far as we know; if the glyph boxes hold more ink upside down, flip
    def ink_in(img):
        total = 0
        for _, x, y, w, h, *_ in rects:
            if w > 0 and h > 0:
                total += int(img[int(y):int(y + h), int(x):int(x + w)].sum())
        return total
    if ink_in(ink[::-1]) > ink_in(ink) * 1.2:
        ink = ink[::-1]
    glyphs, line, by_slot = {}, 0.0, {}
    for i, x, y, w, h, advance, kerning, ascent in rects:
        ch = slot_char(i)
        if not ch or advance <= 0 or w < 0 or h < 0:
            continue
        by_slot[i] = [round(x), round(y), round(w), round(h), round(advance, 2), round(kerning, 2), round(size - ascent, 2)]
        glyphs[ord(ch)] = by_slot[i]
        line = max(line, h)
    for slot, chars in FONT_STAND_INS.items():
        if slot in by_slot:
            for ch in chars:
                glyphs.setdefault(ord(ch), by_slot[slot])
    if ord("A") not in glyphs or ord(" ") not in glyphs:
        return None
    img = Image.new("L", (pow2(tw), pow2(th)), 0)
    img.paste(Image.fromarray(np.ascontiguousarray(ink)), (0, 0))
    return img, glyphs, max(line, size)


def build_font(arch):
    try:
        fnt = arch.read(f"fonts\\{FONT}.fnt")
        name = fnt[12:FNT_HEADER].split(b"\0")[0].decode("latin-1") or FONT
        tex = arch.read(f"fonts\\{name}.tex")
    except (FileNotFoundError, KeyError, OSError):
        return None
    parsed = parse_font(fnt, tex)
    if parsed is None:
        return None
    img, glyphs, line = parsed
    UI_DIR.mkdir(parents=True, exist_ok=True)
    # White with the glyphs in alpha (LA8): citro2d's tint multiplies the texture colour, so the
    # text comes out in the tint colour (an alpha-only A8 font would come out black)
    white = Image.new("L", img.size, 255)
    rgba = Image.merge("RGBA", (white, white, white, img))
    encode_image(rgba, OUT / "ui/font.t3x", "la8")
    return {"file": "ui/font.t3x", "width": img.size[0], "height": img.size[1], "line": line,
            "glyphs": {str(k): v for k, v in glyphs.items()}}


def colors(font_color_section):
    kv = dict(COLOR_DEFAULTS, **{k.lower(): v for k, v in font_color_section.items()})
    out = {}
    for k, v in kv.items():
        parts = [int(p) for p in re.findall(r"-?\d+", v)[:3]]
        if len(parts) == 3:
            out[k[len("color_"):] if k.startswith("color_") else k] = parts
    return out


def build_ui(arch, font_color_section, objects, extra=()):
    icons, count, missing = build_icons(arch, objects, extra)
    ui = {"icons": icons, "colors": colors(font_color_section)}
    atlas = build_atlas(arch)
    if atlas:
        ui["atlas"] = atlas
    font = build_font(arch)
    if font:
        ui["font"] = font
    report = (f"ui: {count} item icons in {len(icons['pages'])} pages"
              f"{f' ({len(missing)} missing, e.g. {missing[0]})' if missing else ''}, "
              f"{len(atlas['pieces']) if atlas else 0} window pieces, "
              f"font {'Magic Cards, %d glyphs' % len(font['glyphs']) if font else 'not found (system font)'}")
    return ui, report
