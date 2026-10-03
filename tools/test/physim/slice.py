"""Cross-sections of a cell's collision mesh (from build/physim/<stem>.raw, written by run.py):
python tools/test/physim/slice.py <stem> x|y|z value [out.png] [marker a,b ...]
(run.py first: physim writes the decoded mesh as <stem>.raw.col)
Draws the triangles cut by the plane, in the other two axes."""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]


def read_col(d):
    """build/physim/<stem>.raw.col: the mesh as physim decoded it (any cell version)."""
    nv, nt = struct.unpack_from("<II", d, 0)
    verts = np.frombuffer(d, dtype="<f4", count=nv * 3, offset=8).reshape(-1, 3)
    idx = np.frombuffer(d, dtype="<u4", count=nt * 3, offset=8 + nv * 12).reshape(-1, 3)
    return verts, idx


def read_collision(raw):
    d = raw
    off = 12 + 20 + 12 + 12 + 16
    n = struct.unpack_from("<I", d, off)[0]
    off += 4 + 64 * n
    n = struct.unpack_from("<I", d, off)[0]
    off += 4
    for _ in range(n):
        nv, ni = struct.unpack_from("<II", d, off + 8)
        off += 72 + nv * 16 + ni * 2 + (2 if ni & 1 else 0)
    nv = struct.unpack_from("<I", d, off)[0]
    offset = np.array(struct.unpack_from("<3f", d, off + 4))
    scale = np.array(struct.unpack_from("<3f", d, off + 16))
    off += 28
    q = np.frombuffer(d, dtype="<i2", count=nv * 3, offset=off).reshape(-1, 3)
    off += nv * 6 + (2 if (nv * 3) & 1 else 0)
    verts = offset + q * scale
    nt, widths = struct.unpack_from("<II", d, off)
    off += 8
    w = widths & 0xFF
    idx = np.frombuffer(d, dtype="<u2" if w == 2 else "<u4", count=nt * 3, offset=off).reshape(-1, 3)
    return verts, idx


def main():
    stem, axis, value = sys.argv[1], "xyz".index(sys.argv[2]), float(sys.argv[3])
    out = sys.argv[4] if len(sys.argv) > 4 else str(ROOT / "build" / "physim" / "slice.png")
    markers = [tuple(map(float, m.split(","))) for m in sys.argv[5:]]
    col = ROOT / "build" / "physim" / (stem + ".raw.col")
    if col.exists():
        verts, idx = read_col(col.read_bytes())
    else:
        verts, idx = read_collision((ROOT / "build" / "physim" / (stem + ".raw")).read_bytes())
    u, v = [a for a in range(3) if a != axis]
    segs = []
    for t in idx:
        p = verts[t]
        s = p[:, axis] - value
        pts = []
        for i in range(3):
            a, b = p[i], p[(i + 1) % 3]
            sa, sb = s[i], s[(i + 1) % 3]
            if (sa <= 0) != (sb <= 0):
                f = sa / (sa - sb)
                pts.append(a + (b - a) * f)
        if len(pts) == 2:
            segs.append((pts[0][[u, v]], pts[1][[u, v]]))
    allp = np.array([q for s in segs for q in s] + [list(m) for m in markers]) if segs else np.zeros((1, 2))
    lo, hi = allp.min(0) - 50, allp.max(0) + 50
    S = 1.0
    W, H = int((hi[0] - lo[0]) * S) + 1, int((hi[1] - lo[1]) * S) + 1
    img = Image.new("RGB", (W, H), (255, 255, 255))
    dr = ImageDraw.Draw(img)
    tr = lambda q: ((q[0] - lo[0]) * S, H - (q[1] - lo[1]) * S)
    for gx in range(int(lo[0] // 100) * 100, int(hi[0]) + 1, 100):
        dr.line([tr((gx, lo[1])), tr((gx, hi[1]))], fill=(230, 230, 230))
        dr.text(tr((gx, lo[1] + 10)), str(gx), fill=(150, 150, 150))
    for gy in range(int(lo[1] // 100) * 100, int(hi[1]) + 1, 100):
        dr.line([tr((lo[0], gy)), tr((hi[0], gy))], fill=(230, 230, 230))
        dr.text(tr((lo[0] + 2, gy)), str(gy), fill=(150, 150, 150))
    for a, b in segs:
        dr.line([tr(a), tr(b)], fill=(0, 0, 0), width=2)
    for m in markers:
        x, y = tr(m)
        dr.ellipse([x - 5, y - 5, x + 5, y + 5], outline=(220, 0, 0), width=2)
    img.save(out)
    print(f"{len(segs)} segments, {axis=} {value}, {u=} {v=}, image {W}x{H}: {out}")


if __name__ == "__main__":
    main()
