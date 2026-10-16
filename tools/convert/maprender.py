"""The world map from above: every outdoor cell rendered straight down by the engine itself (the PC build's software
GPU, its MAPCELL step: the local map's own view), laid over the map level.py drew from the land and the buildings'
outlines, and written again as the data's map.t3x and detail tiles. Morrowind's and OpenMW's maps show the towns as
they look from above; outlines alone left Vivec's cantons blank squares in the sea.

    python tools/convert/maprender.py [--data out/world] [--jobs 6] [--exe build/native/mw3ds-native.exe]

Run after level.py (it needs the converted cells and game.json, and out/map_base/<data folder>.png, the land picture
level.py keeps). Each cell's render is kept in build/maprender/<data folder>/ under a key of its cell files and its
neighbours' (what reaches into its square): a reconversion renders again only the cells that changed. Cells the run
could not render keep the drawn map. Needs the native build (python tools/test/native/build.py); without it the map
stays as level.py drew it.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
CELL = 8192.0
RENDER_PX = 256                  # the local map's target, a cell across
RENDER_VERSION = 2               # bump when the render itself changes (the engine's local map drawing)
CELLS_PER_RUN = 60               # cells one native run renders (one game start each)


def read_game(path):
    raw = path.read_bytes()
    if raw[:4] == b"MWZ1":
        raw = zlib.decompress(raw[8:])
    return json.loads(raw)


def write_game(path, game):
    """As level.py writes it: 'MWZ1', the size, the zlib stream."""
    data = json.dumps(game, separators=(",", ":")).encode("utf-8")
    packed = zlib.compress(data, 9)
    path.write_bytes(b"MWZ1" + struct.pack("<I", len(data)) + packed if len(packed) + 8 < len(data) else data)


def shard_dir(name):
    return "%02x" % (zlib.crc32(name.encode("latin-1")) & 63)


def cell_key(data, files, gx, gy):
    """The cell's and its eight neighbours' cell files: what its square shows."""
    h = hashlib.sha1(str(RENDER_VERSION).encode())
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            name = files.get((gx + dx, gy + dy))
            if not name:
                continue
            for ext in (".cel", ".json"):
                p = data / "cells" / shard_dir(name) / (name + ext)
                if p.exists():
                    h.update(name.encode() + ext.encode())
                    h.update(p.read_bytes())
    return h.hexdigest()


def render_batch(exe, data, sd, cells, wait):
    """One native run: the game started, then MAPCELL for each (gx, gy). Returns {(gx, gy): bmp path} rendered."""
    app = sd / "3ds" / "mw3ds"
    shutil.rmtree(app, ignore_errors=True)
    (app / "m").mkdir(parents=True, exist_ok=True)
    steps = ["3 0 0 0 0 MSG", "0.3 0 0 0 0 GOD"]
    steps += [f"4 0 0 0 0 MAPCELL:{gx}:{gy}:sdmc:/3ds/mw3ds/m/{gx}_{gy}.bmp" for gx, gy in cells]
    mwtools.write_lines(app / "autoinput.txt", steps)
    mwtools.write_lines(app / "start.txt", ["Seyda Neen"])
    env = dict(os.environ, NATIVE_SD=str(sd), MW3DS_DATA=str(data), NATIVE_OLD3DS="0", NATIVE_DRAW="1")
    proc = subprocess.Popen([str(exe)], cwd=ROOT, env=env, creationflags=mwtools.hidden_flags(),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=wait)
    except subprocess.TimeoutExpired:
        mwtools.kill_tree(proc)
    return {(gx, gy): app / "m" / f"{gx}_{gy}.bmp" for gx, gy in cells if (app / "m" / f"{gx}_{gy}.bmp").exists()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default="out/world")
    ap.add_argument("--jobs", type=int, default=max(1, min(8, (os.cpu_count() or 2) - 2)))
    ap.add_argument("--exe", default=mwtools.NATIVE_EXE)
    ap.add_argument("--wait", type=int, default=300, help="seconds one native run may take (a run that could not load stops there)")
    a = ap.parse_args()
    mwtools.load_env()
    data = (ROOT / a.data).resolve()
    exe = (ROOT / a.exe).resolve()
    base_png = data.parent / "map_base" / f"{data.name}.png"
    base_meta = data.parent / "map_base" / f"{data.name}.json"
    game_path = data / "game.json"
    if not exe.exists():
        print(f"maprender: no native build at {a.exe} (tools/test/native/build.py): the map stays as drawn")
        return 0
    if not base_png.exists() or not game_path.exists():
        print(f"maprender: {a.data} has no game.json or no out/map_base picture: run level.py first")
        return 1
    game = read_game(game_path)
    files = {tuple(c["grid"]): c["file"] for c in game["cells"] if not c.get("interior") and "grid" in c}
    cache = ROOT / "build" / "maprender" / data.name
    cache.mkdir(parents=True, exist_ok=True)
    keys = {g: cell_key(data, files, *g) for g in files}
    todo = sorted(g for g in files if not (cache / f"{g[0]}_{g[1]}.{keys[g][:16]}.png").exists())
    print(f"maprender: {len(files)} outdoor cells, {len(files) - len(todo)} unchanged, {len(todo)} to render "
          f"on {a.jobs} processes", flush=True)

    batches = [todo[i:i + CELLS_PER_RUN] for i in range(0, len(todo), CELLS_PER_RUN)]
    done = 0

    def run(k):
        sd = ROOT / "build" / "maprender" / f"sd-{k % a.jobs}"
        return render_batch(exe, data, sd, batches[k], a.wait)

    # (one SD folder per process: batches k and k + jobs never run at once, the pool runs jobs at a time)
    with ThreadPoolExecutor(a.jobs) as pool:
        for k in range(0, len(batches), a.jobs):
            for got in pool.map(run, range(k, min(k + a.jobs, len(batches)))):
                for g, bmp in got.items():
                    for old in cache.glob(f"{g[0]}_{g[1]}.*.png"):
                        old.unlink()
                    Image.open(bmp).convert("RGB").save(cache / f"{g[0]}_{g[1]}.{keys[g][:16]}.png")
                    done += 1
            print(f"maprender: {done} of {len(todo)} rendered", flush=True)
    if done < len(todo):
        print(f"maprender: {len(todo) - done} cells not rendered (they keep the drawn map)")

    # Over the land picture: each render shrunk to the picture's pixels a cell, at its square (north up)
    meta = json.loads(base_meta.read_text())
    img = Image.open(base_png).convert("RGB")
    units, (ox, oy) = meta["units_per_pixel"], meta["origin"]
    px = round(CELL / units)
    laid = 0
    for (gx, gy) in files:
        found = cache / f"{gx}_{gy}.{keys[(gx, gy)][:16]}.png"
        if not found.exists():
            continue
        tile = Image.open(found).convert("RGB").resize((px, px), Image.LANCZOS)
        x = round((gx * CELL - ox) / units)
        y = round(img.size[1] - ((gy + 1) * CELL - oy) / units)
        img.paste(tile, (x, y))
        laid += 1
    sys.path.insert(0, str(Path(__file__).parent))
    os.environ["MW3DS_OUT"] = str(data)
    import worldmap
    labels = [(l["text"], l["x"], l["y"]) for l in game["map"].get("labels", [])]
    img.save(base_png.with_name(f"{data.name}_baked.png"))           # (to look at: not on the card)
    game["map"] = worldmap.encode_map(img, (ox, oy), units, labels)
    write_game(game_path, game)
    print(f"maprender: {laid} cells laid over the map, {a.data}/map.t3x and its detail tiles written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
