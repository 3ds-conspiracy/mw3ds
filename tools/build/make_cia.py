"""Builds build/mw3ds.cia with the converted game data packed into its RomFS.
New 3DS settings: 124MB app memory mode, 804MHz CPU, L2 cache.
    python tools/build/make_cia.py [--no-data] [--data out/world] [--out build/mw3ds-o3ds.cia] [--prebuilt]
  --no-data   the program only (~2 MB): the game reads its data from sdmc:/3ds/mw3ds/data
              (the whole island copied there by card reader); no Wi-Fi update checks either
  --data      pack that data (default out/data, the Balmora area)
  --out       write the CIA there (default build/mw3ds.cia)
  --prebuilt  do not run make: pack the mw3ds.elf and mw3ds.smdh already in the repo root
The CIA's title version (FBI, System Settings > Data Management) is the release's: MW3DS_VERSION, else the release
converter's VERSION.txt, else the git tag on this commit (else the latest tag).
The HOME menu banner's sound is the start of Morrowind's title music, taken from your Data Files when the CIA is packed
(banner_with_tune); without them, the banner's own tune.
The same program runs on both consoles (it takes its Old 3DS profile on an Old 3DS: 64 MB mode, see source/platform/heaps.cpp);
only the data differs (build_cia.py --old3ds converts with quarter-size textures).
"""
import argparse
import array
import re
import struct
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools

ROOT = mwtools.ROOT
DEFS = {
    "APP_TITLE": "MW3DS", "APP_PRODUCT_CODE": "CTR-P-MW3D", "APP_UNIQUE_ID": "0xF8287",
    "APP_ROMFS": "build/romfs", "APP_CATEGORY": "Application", "APP_USE_ON_SD": "true", "APP_ENCRYPTED": "false",
    "APP_MEMORY_TYPE": "Application", "APP_SYSTEM_MODE": "64MB", "APP_SYSTEM_MODE_EXT": "124MB",
    "APP_CPU_SPEED": "804MHz", "APP_ENABLE_L2_CACHE": "true", "APP_VERSION_MAJOR": "0",
}


BANNER = ROOT / "tools" / "build" / "banner.bnr"         # the 3D model (make_art.py) and its own tune
BANNER_WAV = ROOT / "tools" / "build" / "banner.wav"
TUNE = "Music/Special/morrowind title.mp3"                 # under Data Files
TUNE_RATE, TUNE_SECS, TUNE_FADE = 32000, 2.9, 0.6


def app_version():
    """The release's version as text and as the CIA's title version number (major 6 bits, minor 6, micro 4)."""
    text = mwtools.optional_env("MW3DS_VERSION")
    if not text and (ROOT / "VERSION.txt").is_file():
        text = (ROOT / "VERSION.txt").read_text(encoding="utf-8").strip()
    for args in (["--exact-match"], ["--abbrev=0"]):
        if not text:
            r = subprocess.run(["git", "describe", "--tags", *args], cwd=ROOT, capture_output=True, text=True)
            text = r.stdout.strip() if r.returncode == 0 else ""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", text or "")
    if not m:
        return "0.0.0", 0
    major, minor, micro = (int(g) for g in m.groups())
    return f"{major}.{minor}.{micro}", (min(major, 63) << 10) | (min(minor, 63) << 4) | min(micro, 15)


def banner_tune():
    """The banner's sound as (left, right) 16-bit samples at TUNE_RATE: the first TUNE_SECS of Morrowind's title music
    (from the player's own Data Files: it can't ship with the code), faded out and brought up to just under full scale;
    else banner.wav. The HOME menu plays a banner's sound as 32000 Hz stereo 16-bit, under 3 seconds: the 22050 Hz mono
    tune it was given played as a ringing noise."""
    import miniaudio
    morrowind = mwtools.optional_env("MW3DS_MORROWIND_DIR")
    src = Path(morrowind) / "Data Files" / TUNE if morrowind else None
    path = src if src and src.is_file() else BANNER_WAV
    try:
        dec = miniaudio.decode_file(str(path), output_format=miniaudio.SampleFormat.SIGNED16, nchannels=2,
                                    sample_rate=TUNE_RATE)
    except Exception as e:      # (a damaged file: the banner keeps its own sound)
        print(f"banner: no tune ({e})")
        return None
    print(f"banner: sound from {path.name}")
    pcm = dec.samples[:int(TUNE_RATE * TUNE_SECS) * 2]
    frames = len(pcm) // 2
    gain = 0.9 * 32767 / max(1, max(abs(v) for v in pcm))
    fade = int(TUNE_RATE * TUNE_FADE)
    left, right = array.array("h"), array.array("h")
    for i in range(frames):
        g = gain * min(1.0, (frames - i) / fade)
        left.append(max(-32768, min(32767, round(pcm[2 * i] * g))))
        right.append(max(-32768, min(32767, round(pcm[2 * i + 1] * g))))
    return left, right


def cwav(channels, rate):
    """A BCWAV (the 3DS's sound file, as 3dbrew lays it out): PCM16, no loop, each channel's samples in a block."""
    def align(v, a=0x20):
        return (v + a - 1) // a * a
    count, samples = len(channels), len(channels[0])
    table = 0x1C + 4 + 8 * count                 # the channel infos follow the reference table
    info_size = align(table + 0x14 * count)
    span = align(samples * 2)
    data_size = 0x20 + span * count
    info = bytearray(info_size)
    struct.pack_into("<4sIBBHIIII", info, 0, b"INFO", info_size, 1, 0, 0, rate, 0, samples, 0)
    struct.pack_into("<I", info, 0x1C, count)
    for c in range(count):
        # (offsets: a channel's info from the table's start; its samples from the DATA block's body, 8 in)
        struct.pack_into("<HHI", info, 0x20 + 8 * c, 0x7100, 0, 4 + 8 * count + 0x14 * c)
        struct.pack_into("<HHIHHiI", info, table + 0x14 * c, 0x1F00, 0, 0x18 + span * c, 0, 0, -1, 0)
    data = bytearray(data_size)
    struct.pack_into("<4sI", data, 0, b"DATA", data_size)
    for c, ch in enumerate(channels):
        raw = ch.tobytes()
        data[0x20 + span * c:0x20 + span * c + len(raw)] = raw
    head = bytearray(0x40)
    struct.pack_into("<4sHHIIHHHHIIHHII", head, 0, b"CWAV", 0xFEFF, 0x40, 0x02010000, 0x40 + info_size + data_size,
                     2, 0, 0x7000, 0, 0x40, info_size, 0x7001, 0, 0x40 + info_size, data_size)
    return bytes(head + info + data)


def banner_with_tune(dest):
    """banner.bnr with banner_tune's sound in place of its own (the CBMD's sound offset is at 0x84); returns the banner
    to pack: dest, or banner.bnr as it is without a tune."""
    tune = banner_tune()
    if not tune:
        return BANNER
    bnr = BANNER.read_bytes()
    at = struct.unpack_from("<I", bnr, 0x84)[0]
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(bnr[:at] + cwav(tune, TUNE_RATE))
    return dest


def build(data="out/data", out="build/mw3ds.cia", no_data=False):
    """Builds the program and the CIA; returns the exit code."""
    mwtools.load_env()
    made = mwtools.make([], capture=True)
    lines = made.stdout.splitlines()
    for ln in lines:
        if re.search("error|warning", ln, re.IGNORECASE):
            print(ln)
    if made.returncode != 0:
        print(f"make failed (exit code {made.returncode}). Last lines of its output:")
        print("\n".join(lines[-30:]))
        return 1
    return pack(data, out, no_data)


def pack(data="out/data", out="build/mw3ds.cia", no_data=False):
    """Packs the program already built (mw3ds.elf and mw3ds.smdh in the repo root) and the data into the CIA;
    returns the exit code. The release converter calls this with a prebuilt program (tools/release)."""
    mwtools.load_env()
    for f in ("mw3ds.elf", "mw3ds.smdh"):
        if not (ROOT / f).is_file():
            print(f"{f} not found in {ROOT}: build the program first (tools/build/make.py)")
            return 1
    staged = ROOT / "build" / "romfs" / "data"
    shutil.rmtree(staged, ignore_errors=True)
    # (the RSF's RomFs root: empty for the program-only CIA, which makerom wants to exist all the same)
    staged.parent.mkdir(parents=True, exist_ok=True)
    if not no_data:
        mwtools.copy_tree_linked(ROOT / data, staged)
    makerom_name = "makerom.exe" if mwtools.IS_WINDOWS else "makerom"
    makerom = Path(mwtools.get_env("MW3DS_CTR_TOOLS_DIR", f"the folder with {makerom_name}")) / makerom_name
    args = [str(makerom), "-f", "cia", "-o", str(out), "-elf", "mw3ds.elf", "-rsf", "tools/build/mw3ds.rsf",
            "-icon", "mw3ds.smdh", "-banner", str(banner_with_tune(ROOT / "build" / "banner.bnr")), "-exefslogo",
            "-target", "t"]
    args += [f"-D{k}={v}" for k, v in DEFS.items()]
    ver, number = app_version()
    args += ["-ver", str(number)]
    print(f"version {ver}")
    code = subprocess.run(args, cwd=ROOT).returncode
    # The staged copy of the data is only makerom's input: packed into the CIA, it would be a third 1 GB copy on disk
    shutil.rmtree(staged, ignore_errors=True)
    if code != 0:
        print("makerom failed")
        return 1
    cia = ROOT / out
    print(f"built {cia} ({cia.stat().st_size / 1048576:.1f} MB)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-data", action="store_true")
    ap.add_argument("--data", default="out/data")
    ap.add_argument("--out", default="build/mw3ds.cia")
    ap.add_argument("--prebuilt", action="store_true")
    a = ap.parse_args()
    return (pack if a.prebuilt else build)(a.data, a.out, a.no_data)


if __name__ == "__main__":
    sys.exit(main())
