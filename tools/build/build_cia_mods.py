"""Same as build_cia.py (converts your Morrowind install and builds the CIA), with the community optimization mods
(Morrowind Optimization Patch, Project Atlas) laid over the game files: fewer, bigger batches per frame. The mods are
git submodules of this repository (mods/, pinned to a commit each): they are downloaded the first time (needs git and
a network, about 550 MB) by `git submodule update --init`, which this script runs. Your Morrowind install is never
modified, and nothing from the mods is part of this repository (a submodule is only a link to the other repository).
    python tools/build/build_cia_mods.py                 whole island, New 3DS (out/data, build/mw3ds.cia)
    python tools/build/build_cia_mods.py --old3ds        whole island, Old 3DS (out/o3ds, build/mw3ds-o3ds.cia)
    python tools/build/build_cia_mods.py --small         only the area around Balmora
    python tools/build/build_cia_mods.py --skip-convert  reuse the converted data (it must have been converted with the mods)
    python tools/build/build_cia_mods.py --fetch-only    just download the mods and print the overlay folders
The plain build (build_cia.py) writes the same folders: switching between the two means converting again.
"""
import sys

import build_cia

if __name__ == "__main__":
    sys.exit(build_cia.main(mods=True))
