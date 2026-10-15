"""Copied into the release converter's python folder by tools/release/make_release.py; Python runs it at start.
The embeddable Python runs isolated (its python3XX._pth file): it leaves the script's own folder off sys.path and
ignores PYTHONUNBUFFERED. The converters import their neighbours and the wizard reads their output as it comes,
so this puts both back as a normal Python install has them."""
import os
import sys

if len(sys.argv) > 0 and sys.argv[0].endswith(".py") and os.path.isfile(sys.argv[0]):
    sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))

for stream in (sys.stdout, sys.stderr):
    if stream is not None and hasattr(stream, "reconfigure"):
        try:
            stream.reconfigure(line_buffering=True)
        except (OSError, ValueError):
            pass
