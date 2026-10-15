"""Renders the CIA's HOME menu art from tools/build/art/emblem.py: the icon and the 3D banner.
    python tools/build/make_art.py [--skip-render]
Writes icon.png (48 x 48, the Makefile turns it into mw3ds.smdh), tools/build/banner.png (256 x 128, the banner
as a flat picture, for pages that show it) and tools/build/banner.bnr: the 3D banner, a CGFX model the HOME menu
draws in stereo (title in front, emblem behind it, stone wall at the back), with tools/build/banner.wav. The outputs
are committed: building the game needs neither Blender nor this script, only changing the art does.
  --skip-render  keep the Blender renders in build/art from the last run and only redo the conversion
How the banner is made: emblem.py frames the scene with the HOME menu's camera and renders each layer alone; each
render is projected onto that layer's low-poly mesh (exported as glTF), and pycgfx converts the glTF to CGFX.
The textures are RGB8 (pycgfx's default, RGBA4, bands the gold) and the materials show their texture unlit, since
the light and shading are already in the renders.
Needs Blender 3.6 (MW3DS_BLENDER in .env, or a standard install), a checkout of pycgfx
(https://github.com/skyfloogle/pycgfx, MW3DS_PYCGFX in .env) with its gltflib dependency (pip install gltflib),
and bannertool next to makerom in MW3DS_CTR_TOOLS_DIR (https://github.com/Epicpkmn11/bannertool/releases).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
ART = ROOT / "tools" / "build" / "art"
WORK = ROOT / "build" / "art"
BANNER_PNG = ROOT / "tools" / "build" / "banner.png"
BANNER_WAV = ROOT / "tools" / "build" / "banner.wav"
BANNER_BNR = ROOT / "tools" / "build" / "banner.bnr"
ICON_PNG = ROOT / "icon.png"
CGFX_LIMIT = 0x80000  # the HOME menu refuses a banner model over 512 KB


def find_blender():
    named = mwtools.optional_env("MW3DS_BLENDER")
    if named:
        return Path(named)
    if mwtools.IS_WINDOWS:
        found = sorted(Path(os.environ.get("ProgramFiles", r"C:\Program Files"), "Blender Foundation").glob("*/blender.exe"))
        if found:
            return found[-1]
    raise SystemExit("Blender not found: set MW3DS_BLENDER in .env to the blender executable")


def find_bannertool():
    tools = Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", "the folder with makerom (and bannertool)"))
    name = "bannertool.exe" if mwtools.IS_WINDOWS else "bannertool"
    found = sorted(tools.rglob(name))
    if not found:
        raise SystemExit(f"{name} not found under {tools}: get it from https://github.com/Epicpkmn11/bannertool/releases")
    return found[-1]


def blender_run(blender, *scene_args):
    args = [str(blender), "-b", "--factory-startup", "-P", str(ART / "emblem.py"), "--",
            "--font", str(ART / "Cinzel-650.ttf"), *scene_args]
    done = subprocess.run(args, capture_output=True, text=True)
    if done.returncode != 0 or "Traceback" in done.stderr + done.stdout:
        print(done.stdout[-3000:], done.stderr[-3000:])
        raise SystemExit(f"Blender failed: {' '.join(scene_args)}")
    return done.stdout


def box_sum(a, r):
    """Sum over a (2r+1)-square window around each pixel, zero outside the image."""
    p = np.pad(a, ((r + 1, r), (r + 1, r), (0, 0)))
    c = p.cumsum(0).cumsum(1)
    n = 2 * r + 1
    return c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]


def bleed(rgba):
    """RGB of an RGBA render with the colour at the shape's edges carried out over the transparent pixels, so a
    texture filtered or sampled just outside its shape never picks up the black of empty pixels."""
    a = np.asarray(rgba, dtype=np.float64) / 255
    alpha = a[..., 3:4]
    prem = a[..., :3] * alpha
    rgb = np.where(alpha > 0, prem / np.maximum(alpha, 1e-6), 0)
    done = alpha[..., 0] > 0
    for r in (1, 2, 4, 8, 16, 32, 64, 128):
        if done.all():
            break
        w = box_sum(alpha, r)
        fill = box_sum(prem, r) / np.maximum(w, 1e-9)
        new = ~done & (w[..., 0] > 1e-6)
        rgb[new] = fill[new]
        done |= new
    return Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def make_textures():
    """Crops each layer's render to its mesh's UV rectangle and writes it over the glTF's placeholder texture."""
    layout = json.loads((WORK / "layers.json").read_text())
    for layer, info in layout.items():
        im = bleed(Image.open(WORK / f"{layer}_render.png").convert("RGBA"))
        x0, y0, x1, y1 = info["crop"]
        box = (x0 * im.width, (1 - y1) * im.height, x1 * im.width, (1 - y0) * im.height)
        im.resize(tuple(info["size"]), Image.LANCZOS, box=box).save(WORK / f"{layer}.png")


def make_cgfx(out):
    """glTF to CGFX with pycgfx, with RGB8 textures and unlit materials."""
    pycgfx_dir = Path(mwtools.get_env("MW3DS_PYCGFX", "a checkout of https://github.com/skyfloogle/pycgfx"))
    sys.path.insert(0, str(pycgfx_dir))
    import gltflib
    import main as pycgfx
    from cgfx import swizzler
    from cgfx.txob import TextureFormat

    to_txob = swizzler.to_txob
    swizzler.to_txob = lambda im, format=None, mipmaps=1: to_txob(im, TextureFormat.RGB8, mipmaps)
    cgfx = pycgfx.convert_gltf(gltflib.GLTF.load(str(WORK / "banner.gltf"), load_file_resources=True))
    model = cgfx.data.models["COMMON"]
    for name in model.materials:
        stages = model.materials[name].fragment_shader.texture_combiners
        # stage 0: the texture as is (pycgfx multiplies it by the vertex colour); stages 1 and 2 pass it on instead
        # of multiplying in the HOME menu's diffuse light and adding its specular
        stages[0].src_rgb = stages[0].src_alpha = 0x003
        stages[0].combine_rgb = stages[0].combine_alpha = 0
        for stage in stages[1:3]:
            stage.src_rgb = stage.src_alpha = 0x00F
            stage.combine_rgb = stage.combine_alpha = 0
    data = pycgfx.write(cgfx)
    if len(data) > CGFX_LIMIT:
        raise SystemExit(f"the banner model is {len(data)} bytes, over the HOME menu's {CGFX_LIMIT}")
    out.write_bytes(data)
    print(f"banner model: {len(data) // 1024} KB of {CGFX_LIMIT // 1024}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-render", action="store_true")
    a = ap.parse_args()
    mwtools.load_env()
    WORK.mkdir(parents=True, exist_ok=True)
    if not a.skip_render:
        blender = find_blender()
        for mode, dest, size in (("banner", BANNER_PNG, (256, 128)), ("icon", ICON_PNG, (48, 48))):
            full = WORK / f"{mode}.png"
            print(f"rendering the {mode} (a minute or so)")
            blender_run(blender, "--mode", mode, "--out", str(full))
            Image.open(full).convert("RGB").resize(size, Image.LANCZOS).save(dest)
            print(f"wrote {dest.relative_to(ROOT)}")
        print("rendering the 3D banner's layers (a few minutes)")
        for line in blender_run(blender, "--mode", "model", "--out-dir", str(WORK)).splitlines():
            if line.startswith("layer "):
                print(" ", line)
    make_textures()
    cgfx = WORK / "banner.cgfx"
    make_cgfx(cgfx)
    done = subprocess.run([str(find_bannertool()), "makebanner", "-ci", str(cgfx), "-a", str(BANNER_WAV),
                           "-o", str(BANNER_BNR)], capture_output=True, text=True)
    if done.returncode != 0 or not BANNER_BNR.exists():
        print(done.stdout, done.stderr)
        return 1
    print(f"wrote {BANNER_BNR.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
