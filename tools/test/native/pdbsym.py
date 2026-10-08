"""The native build's crash offsets as function names: native_crash.txt (written by native_crash.inc when the native exe
faults) lists offsets from the exe's base; this reads the public symbols straight from the .pdb zig writes next to the
exe (MSF 7.0, S_PUB32 records) and names the function each offset falls in. No debugger or LLVM tools needed.

  python tools/test/native/pdbsym.py build/native/mw3ds-native.exe 194bd0 1760b1 ...
  python tools/test/native/pdbsym.py build/native/mw3ds-native.exe --prof native_prof.txt [rows]   (NATIVE_PROF=1 samples)
Use the exe that crashed: offsets from another build mean nothing.
"""
import bisect
import struct
import sys


def msf_streams(data):
    assert data[:32].startswith(b"Microsoft C/C++ MSF 7.00")
    bs, _, nblocks, dir_bytes, _, dir_map = struct.unpack_from("<6I", data, 32)
    nmap = (dir_bytes + bs - 1) // bs
    map_blocks = [dir_map]
    dir_block_ids = []
    for mb in map_blocks:
        dir_block_ids += struct.unpack_from("<%dI" % ((nmap * 4 + bs - 1) // bs * bs // 4), data, mb * bs)[:nmap]
    d = b"".join(data[b * bs:(b + 1) * bs] for b in dir_block_ids)[:dir_bytes]
    n = struct.unpack_from("<I", d, 0)[0]
    sizes = struct.unpack_from("<%dI" % n, d, 4)
    pos = 4 + 4 * n
    streams = []
    for s in sizes:
        if s == 0xFFFFFFFF:
            streams.append(b"")
            continue
        k = (s + bs - 1) // bs
        ids = struct.unpack_from("<%dI" % k, d, pos)
        pos += 4 * k
        streams.append(b"".join(data[b * bs:(b + 1) * bs] for b in ids)[:s])
    return streams


def pe_sections(exe):
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    off = pe + 24 + opt
    return [struct.unpack_from("<I", exe, off + 40 * i + 12)[0] for i in range(nsec)]


exe = open(sys.argv[1], "rb").read()
secs = pe_sections(exe)
streams = msf_streams(open(sys.argv[1][:-4] + ".pdb", "rb").read())
dbi = streams[3]
sym_stream = struct.unpack_from("<H", dbi, 20)[0]
recs = streams[sym_stream]
syms = []
i = 0
while i + 4 <= len(recs):
    ln, kind = struct.unpack_from("<HH", recs, i)
    if kind == 0x110E:            # S_PUB32
        _, off, seg = struct.unpack_from("<IIH", recs, i + 4)
        name = recs[i + 14:recs.index(b"\0", i + 14)].decode("latin-1")
        if 1 <= seg <= len(secs):
            syms.append((secs[seg - 1] + off, name))
    i += ln + 2
syms.sort()
addrs = [a for a, _ in syms]
print("%d public symbols" % len(syms))
if len(sys.argv) > 3 and sys.argv[2] == "--prof":
    # native_prof.txt ("offset count" lines, NATIVE_PROF=1): self samples by function, biggest first
    by = {}
    total = 0
    for line in open(sys.argv[3]):
        a, n = line.split()
        a = int(a, 16)
        k = bisect.bisect_right(addrs, a) - 1
        name = syms[k][1] if k >= 0 else "?"
        by[name] = by.get(name, 0) + int(n)
        total += int(n)
    for name, n in sorted(by.items(), key=lambda kv: -kv[1])[:int(sys.argv[4]) if len(sys.argv) > 4 else 40]:
        print("%6.2f%% %7d  %s" % (100.0 * n / total, n, name))
    print("total %d samples" % total)
    sys.exit(0)
for h in sys.argv[2:]:
    a = int(h, 16)
    k = bisect.bisect_right(addrs, a) - 1
    print("+0x%x  %s+0x%x" % (a, syms[k][1] if k >= 0 else "?", a - addrs[k] if k >= 0 else 0))
