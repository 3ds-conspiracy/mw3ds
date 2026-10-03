"""Per-instance rules: engine behaviour that particular placed things need, whether or not a script asks.

  - creatures that trade / train / talk (AIDT services, or dialogue with them as the speaker): talking to a
    living creature must open dialogue (Creeper, the mudcrab merchant)
  - actors placed in the air (no floor under them within 64 units in the cell's own geometry is not known
    here; instead: actors whose cell also holds a marker-box collision object with a script that disables
    it, and actors disabled at start that a script enables and then removes the ground under): gravity
  - placed soul gems with a soul (XSOL), placed items with charge (XCHG): must keep them
  - containers flagged Respawn (CNDT / FLAG bit): restock
Writes build/instancecheck.md.
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mwfiles import cell_refs, load_db

ROOT = Path(__file__).resolve().parents[1]


def main():
    db = load_db()
    objs = db["objects"]
    out = ["# Instance rules (tools/instancecheck.py)", ""]

    # creatures people talk to
    speakers = set()
    for dial in db["dialogue"]:
        for info in dial["infos"]:
            who = info.zstr("ONAM")
            if who:
                speakers.add(who.lower())
    talk = []
    for oid, rec in objs.items():
        if rec.tag != "CREA":
            continue
        aidt = rec.get("AIDT")
        services = struct.unpack_from("<I", aidt, 8)[0] if aidt and len(aidt) >= 12 else 0
        services &= 0x3FFFF
        if services or oid in speakers:
            talk.append(f"{oid} ({rec.zstr('FNAM')}): services {services:#x}, dialogue {'yes' if oid in speakers else 'no'}")
    out += [f"## Creatures to talk to / trade with: {len(talk)}", ""] + [f"- {t}" for t in sorted(talk)] + [""]

    # placed refs with soul / charge / respawning containers / actors high up
    souls, charges, respawn = [], [], []
    for key, cell in db["CELL"].items():
        cname = cell.zstr("NAME") or str(key)
        cur = None
        for tag, d in cell.subs:
            if tag == "NAME":
                cur = d.split(b"\0", 1)[0].decode("latin-1")
            elif tag == "XSOL" and cur:
                souls.append(f"{cur} in {cname}: {d.split(bytes(1), 1)[0].decode('latin-1')}")
            elif tag == "XCHG" and cur:
                charges.append(f"{cur} in {cname}: {struct.unpack('<f', d)[0]:.0f}")
    for oid, rec in objs.items():
        if rec.tag == "CONT":
            flags = rec.get("FLAG")
            if flags and struct.unpack("<I", flags)[0] & 2:
                respawn.append(oid)
    out += [f"## Placed soul gems holding a soul (XSOL): {len(souls)}", ""] + [f"- {s}" for s in souls[:40]] + [""]
    out += [f"## Placed enchanted items with charge (XCHG): {len(charges)}", ""] + [f"- {s}" for s in charges[:20]] + [""]
    out += [f"## Containers that restock (respawn flag): {len(respawn)}", ""] + [", ".join(sorted(respawn)[:60])] + [""]

    (ROOT / "build" / "instancecheck.md").write_text("\n".join(out), encoding="utf-8")
    print(f"talkable creatures {len(talk)}, placed souls {len(souls)}, placed charges {len(charges)}, "
          f"respawning containers {len(respawn)}")


if __name__ == "__main__":
    main()
