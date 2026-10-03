"""The converted game data (out/world/game*.json, zlib-packed with an 8-byte 'MWZ1' + length header), read for
the spec-driven tests: the oracle in openmw_specgen.py takes effect base costs, flags, ingredients, apparatus
quality and GMSTs from the same files the engine loads, so a test checks the engine's logic, not the data."""
import json
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, data="out/world"):
    raw = (ROOT / data / name).read_bytes()
    if raw[:4] == b"MWZ1":
        raw = zlib.decompress(raw[8:])
    return json.loads(raw.decode("utf-8"))
