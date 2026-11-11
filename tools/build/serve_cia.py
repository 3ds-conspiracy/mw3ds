"""Serve the built CIAs over HTTP with a QR code for each, for FBI's "Remote Install".

Usage:
    python tools/build/serve_cia.py                  serves build/mw3ds.cia and build/mw3ds-o3ds.cia
    python tools/build/serve_cia.py --port 8080 --host 192.168.1.50
    python tools/build/serve_cia.py build/other.cia  serves the files given instead

On the 3DS: FBI > Remote Install > Scan QR Code, then scan the code on the page (or in the terminal).
The PC and the 3DS must be on the same network. The page is at http://<host>:<port>/.
Needs `segno` (pip install segno) for the QR codes.
"""
import argparse
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    import segno
except ImportError:
    segno = None

DEFAULTS = [
    ("New 3DS + Tribunal + Bloodmoon", "build/mw3ds-tribunal-bloodmoon.cia"),
    ("Old 3DS + Tribunal + Bloodmoon", "build/mw3ds-o3ds-tribunal-bloodmoon.cia"),
    ("New 3DS", "build/mw3ds.cia"),
    ("Old 3DS", "build/mw3ds-o3ds.cia"),
]


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cia", nargs="*", help="CIA files (default: the New and Old 3DS builds)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default=None, help="address the 3DS should use (default: this PC's LAN address)")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[2]
    if args.cia:
        entries = [(Path(c).stem, c) for c in args.cia]
    else:
        entries = DEFAULTS
    files = {}
    for label, rel in entries:
        p = Path(rel)
        p = p if p.is_absolute() else root / p
        if not p.is_file():
            sys.exit(f"missing: {p}")
        files[p.name] = (label, p)

    sys.stdout.reconfigure(encoding="utf-8")  # the terminal QR uses block characters
    host = args.host or lan_ip()
    base = f"http://{host}:{args.port}"
    if segno is None:
        print("segno is not installed: no QR codes (pip install segno)")

    def qr_svg(url):
        return segno.make(url, error="m").svg_inline(scale=6, border=2) if segno else ""

    rows = []
    for name, (label, p) in files.items():
        url = f"{base}/{name}"
        mb = p.stat().st_size / 1e6
        rows.append(f'<div class="c"><h2>{label}</h2>{qr_svg(url)}<p><a href="/{name}">{url}</a><br>{mb:,.0f} MB</p></div>')
        print(f"{label}: {url}  ({mb:,.0f} MB)")
        if segno:
            segno.make(url, error="m").terminal(compact=True)
    page = ("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
            "<title>MW3DS CIAs</title><style>body{font-family:sans-serif;background:#fff;color:#000}"
            ".c{display:inline-block;margin:1em;text-align:center}svg{width:260px;height:260px}</style>"
            "<h1>MW3DS</h1><p>FBI &gt; Remote Install &gt; Scan QR Code.</p>" + "".join(rows)).encode()

    class Handler(BaseHTTPRequestHandler):
        def do_HEAD(self):
            self.do_GET(head=True)

        def do_GET(self, head=False):
            path = self.path.split("?")[0].lstrip("/")
            if path in ("", "index.html"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                if not head:
                    self.wfile.write(page)
            elif path in files:
                p = files[path][1]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(p.stat().st_size))
                self.end_headers()
                if head:
                    return
                try:
                    with open(p, "rb") as f:
                        while True:
                            b = f.read(1 << 20)
                            if not b:
                                break
                            self.wfile.write(b)
                except (ConnectionError, OSError):
                    pass
            else:
                self.send_error(404)

        def log_message(self, fmt, *a):
            print(self.address_string(), fmt % a)

    print(f"page: {base}/   (Ctrl+C to stop)")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
