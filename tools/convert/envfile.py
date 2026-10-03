"""Reads the repo's .env (see .env.example) into os.environ; variables already set win over the file."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load():
    env = ROOT / ".env"
    if not env.is_file():
        return
    for line in env.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip().strip('"').strip("'")
        if value:
            os.environ.setdefault(key.strip(), value)


def require(name, what):
    """The value of environment variable `name` (from the shell or .env), or exit saying how to set it."""
    load()
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"{name} is not set: {what}. Put it in .env (copy .env.example) or the environment.")
    return value


load()
