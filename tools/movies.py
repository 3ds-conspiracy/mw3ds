"""Morrowind's movies (Data Files\\Video\\*.bik) for the 3DS: the intro at a new game, the ending, Dagoth Ur's
cavern, the logos. Bink is decoded with ffmpeg (the imageio-ffmpeg package brings one).

  data/movies/<name>.mwv   'MWV1', u32 frames, u32 fps x 100, u32 w, u32 h (the picture inside the texture),
                           then per frame: u32 size, a .t3x (tex3ds, ETC1, 256 x 256 holding 256 x 192)
  data/music/movie_<name>.snd   its sound, as the other music (IMA ADPCM 'SND2', mono)
game.json "movies": {"mw_intro": {"file": ..., "sound": ...}, ...}

  python tools/movies.py        (MW3DS_OUT picks the data folder)
"""
import audioop
import io
import re
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

from textures import OUT, tex3ds_path

MOVIES = ["mw_intro", "mw_end", "mw_cavern", "bethesda logo", "mw_logo", "mw_credits"]
FPS = 10
SIZE = (256, 192)                  # 4:3, drawn at 320 x 240 on the top screen
RATE = 22050


def ffmpeg():
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def movie_name(name):
    return re.sub(r"[^a-z0-9_]", "_", name.lower())


def convert(src, name):
    ff = ffmpeg()
    stem = movie_name(name)
    out = OUT / "movies" / f"{stem}.mwv"
    out.parent.mkdir(parents=True, exist_ok=True)
    # frames as raw RGB
    w, h = SIZE
    raw = subprocess.run([ff, "-v", "error", "-i", str(src), "-vf", f"fps={FPS},scale={w}:{h}", "-f", "rawvideo",
                          "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    n = len(raw) // (w * h * 3)
    frames = []
    tex3ds = tex3ds_path()
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(n):
            img = Image.frombytes("RGB", (w, h), raw[k * w * h * 3:(k + 1) * w * h * 3])
            canvas = Image.new("RGB", (256, 256))
            canvas.paste(img, (0, 0))
            png, t3x = Path(tmp) / "f.png", Path(tmp) / "f.t3x"
            canvas.save(png)
            subprocess.run([tex3ds, "-f", "etc1", "-o", str(t3x), str(png)], check=True, capture_output=True)
            frames.append(t3x.read_bytes())
    with open(out, "wb") as f:
        f.write(b"MWV1" + struct.pack("<IIII", len(frames), FPS * 100, w, h))
        for fr in frames:
            f.write(struct.pack("<I", len(fr)) + fr)
    # the sound (the logo has none)
    pcm = subprocess.run([ff, "-v", "error", "-i", str(src), "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
                         capture_output=True).stdout
    if not pcm:
        return {"file": f"movies/{stem}.mwv", "sound": "", "frames": len(frames), "bytes": out.stat().st_size}
    snd = OUT / "music" / f"movie_{stem}.snd"
    snd.parent.mkdir(parents=True, exist_ok=True)
    samples = len(pcm) // 2
    adpcm, _ = audioop.lin2adpcm(pcm, 2, None)
    with open(snd, "wb") as f:
        f.write(b"SND2" + struct.pack("<II", RATE, samples) + adpcm)
    return {"file": f"movies/{stem}.mwv", "sound": f"movie_{stem}.snd", "frames": len(frames),
            "bytes": out.stat().st_size}


def build_movies(data_files):
    """Converts each movie once (an existing .mwv is kept: minutes of work)."""
    video = Path(data_files) / "Video"
    out = {}
    for name in MOVIES:
        src = video / f"{name}.bik"
        if not src.exists():
            continue
        stem = movie_name(name)
        mwv, snd = OUT / "movies" / f"{stem}.mwv", OUT / "music" / f"movie_{stem}.snd"
        if mwv.exists():
            out[stem] = {"file": f"movies/{stem}.mwv", "sound": snd.name if snd.exists() else "",
                         "frames": struct.unpack_from("<I", mwv.read_bytes()[:8], 4)[0], "bytes": mwv.stat().st_size}
            continue
        try:
            out[movie_name(name)] = convert(src, name)
        except Exception as e:
            print(f"  movie {name}: {e}")
    return out


if __name__ == "__main__":
    import json
    from mwfiles import DATA_FILES
    info = build_movies(DATA_FILES.parent if DATA_FILES.suffix else DATA_FILES)
    for k, v in info.items():
        print(f"{k}: {v['frames']} frames, {v['bytes'] // 1024} KB")
    (OUT / "movies" / "movies.json").write_text(json.dumps(info))
