"""Sends a built CIA to a 3DS running ftpd (plain FTP, anonymous login), into its /cias/ folder for FBI to install.
    python tools/build/upload_cia.py build/mw3ds-tribunal-bloodmoon.cia            to MW3DS_3DS_FTP (host:port)
    python tools/build/upload_cia.py build/mw3ds-o3ds.cia --to 192.168.1.50:5000 --name mw3ds-o3ds-test.cia
  --to    the console's address and port as ftpd shows them (default MW3DS_3DS_FTP from .env)
  --name  the file's name on the console (default the local one); a name already there is refused, not replaced
Uses curl (one plain upload, ~1 MB/s: a 1.4 GB CIA takes 20-25 minutes). Python's ftplib STOR of a large file was
refused at once by the console's ftpd ("553 No such device") while curl's goes through. A half-sent file is left
there: delete it and send again rather than resuming, which crawls.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mwtools


def listing(to):
    """The names in the console's /cias/ folder."""
    for attempt in range(3):          # (right after a long upload the console can be slow to answer)
        r = subprocess.run(["curl", "-s", "-m", "60", "--user", "anonymous:", f"ftp://{to}/cias/", "--list-only"],
                           capture_output=True, text=True)
        if r.returncode == 0:
            return {line.strip().rsplit("/", 1)[-1] for line in r.stdout.splitlines() if line.strip()}
    raise SystemExit(f"no answer from ftp://{to}/ (curl {r.returncode}): is ftpd running on the console?")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cia")
    ap.add_argument("--to", default="")
    ap.add_argument("--name", default="")
    args = ap.parse_args()
    if not shutil.which("curl"):
        raise SystemExit("curl not found")
    cia = Path(args.cia)
    if not cia.is_file():
        raise SystemExit(f"no {cia}")
    to = args.to or mwtools.get_env("MW3DS_3DS_FTP", "the 3DS's ftpd address, host:port (such as 192.168.1.50:5000)")
    name = args.name or cia.name
    if name in listing(to):
        raise SystemExit(f"/cias/{name} is already on the console: pick another --name, or delete it there first")
    print(f"sending {cia} ({cia.stat().st_size / 1e6:.0f} MB) to ftp://{to}/cias/{name}", flush=True)
    r = subprocess.run(["curl", "-S", "--user", "anonymous:", "-T", str(cia.resolve()), f"ftp://{to}/cias/{name}",
                        "-w", "sent %{size_upload} bytes in %{time_total} s\n"])
    if r.returncode != 0:
        raise SystemExit(f"upload failed (curl {r.returncode})")
    if name not in listing(to):
        raise SystemExit(f"/cias/{name} is not on the console after the upload")
    print("done: install it with FBI (SD > cias)")


if __name__ == "__main__":
    main()
