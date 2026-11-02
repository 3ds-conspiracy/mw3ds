"""Which textures get smaller copies (mipmaps), and how they are made. Used by textures.py's encode.

Kept out of textures.py: that file is part of the cell cache's key (convert_cell.py), and which pictures carry
mipmaps changes no cell. A change here re-encodes the textures (ENCODE_VERSION in the texture cache) and nothing else.

The modes (level.py --mips):
    all    every texture of 64 texels or more on both sides (a New 3DS)
    few    only the ground's (an Old 3DS: the most screen drawn from the fewest KB)
    none   no texture
"""
import struct

import mwfiles

# Bumped when what a texture is encoded with changes: every texture is encoded again
ENCODE_VERSION = 1
# A texture under this many texels on a side keeps one level. A strip (a step's edge, a moulding, a column: 128 x 32,
# 32 x 256) had three, each unlike the next, and the console showed a step's face split between two of them, its
# ETC1 blocks shifted along the triangle's edge (the Redoran steps, Fort Frostmoth's gate). At 2 to 4 KB it sits in the
# GPU's texture cache whole: it never needed smaller copies for speed
MIN_MIP_SIDE = 64
# Textures that keep one level whatever their size: their smaller copies showed on the console as triangles, one
# triangle of a face drawn from another level than the next. Fort Frostmoth's square pillars (64 x 256, on the edge of
# the rule above) and the Imperial guards' boots, up the shin and the foot
NO_MIPS = {
    "tx_imp_colwide_01",
    "tx_a_imperial_a_boot",
    "tx_a_imperial_shoe",
}

_land = None


def land_textures():
    """The ground's textures (the LTEX records of the load order), as lower-case file stems."""
    global _land
    if _land is None:
        _land = set()
        for path in mwfiles.load_order():
            for r in mwfiles.read_records(path, wanted={"LTEX"}):
                name = (r.zstr("DATA") or "").lower().replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0]
                if name:
                    _land.add(name)
    return _land


def wants_mips(mode, path, sheet):
    """Whether this texture should carry smaller copies in this mode (before its size is known: see mip_side_ok)."""
    stem = path.lower().replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0]
    if stem in NO_MIPS:
        return False
    if mode == "all":
        return True
    if mode == "few":
        # (not Project Atlas's sheets: in Balmora their copies were 143 KB of the 220 the Old 3DS's mipmaps cost, with
        # its linear memory at the 5 MB where it holds cells back, and a trim packed in a sheet ran into the canal walk
        # beside it far off)
        return stem in land_textures()
    return False


def mip_side_ok(w, h):
    return min(w, h) >= MIN_MIP_SIDE


def mip_filter(sheet):
    """The filter tex3ds shrinks each level with. A sheet's pictures are stacked squares: a box filter averages each
    2 x 2 inside one picture (their edges fall on multiples of every level's step until a picture is a texel), where the
    triangle filter's wider reach took in the next picture's colours: Hlaalu's wall bled into the dark trim packed
    under it, far walls showing dark patches. Others keep the triangle filter, a little softer"""
    return "box" if sheet else "triangle"


def cache_key(want):
    """What a texture cache entry records of how it was encoded: an entry with another key is encoded again."""
    return f"{ENCODE_VERSION}:{'mips' if want else 'one'}"
