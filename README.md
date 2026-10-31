# MW3DS: Morrowind for the 3DS

Play *The Elder Scrolls III: Morrowind* on a **Nintendo 3DS**. MW3DS is a from-scratch engine written in C++ for the 3DS.
It runs the base game on real hardware and in the [Azahar](https://azahar-emu.org/) emulator.

**It supports both the Old 3DS / 2DS and the New 3DS / New 2DS XL, with a separate build for each.** The converter
(or the build script) makes a CIA for the console you choose: the New 3DS build has the full textures, the Old 3DS
build quarter-size ones to fit its memory. The engine also knows which console it is on and takes a lighter profile on
an Old 3DS (see [On an Old 3DS](#on-an-old-3ds)). Install the build made for your console: the New 3DS's on an Old 3DS
shows a screen that says so and quits. It is under development: expect rough edges, and please report what you see (see
[Testing and reporting](#testing-and-reporting)).

> [!IMPORTANT]
> **For testing and research only. You need your own legally obtained copy of Morrowind.**
> This repository contains no Morrowind content. The tools convert the game files from your own install, on your own
> machine. The converted data and any build made from it contain copyrighted material: keep them to yourself and never
> share or upload them. MW3DS is an unofficial fan project, not affiliated with or endorsed by Bethesda Softworks or
> ZeniMax Media.

## The converter (Windows, no setup)

The easiest way to get a CIA. You need no git, Python or devkitPro: just Windows, your Morrowind install (Steam, GOG or
disc) and an internet connection.

1. Download `MW3DS-Converter-<version>.zip` from [Releases](https://github.com/3ds-conspiracy/mw3ds/releases).
2. Unzip it to a short folder with about 5 GB free, such as `C:\MW3DS` (deep folders under Downloads or OneDrive can
   pass Windows' path length limit).
3. Run `MW3DS Converter.exe`. Windows may warn about an unknown publisher: **More info**, then **Run anyway**.
4. Pick your Morrowind folder, your console (New 3DS or Old 3DS) and the game's language, then **Start**. The first
   run takes an hour or more and downloads ffmpeg (about 30 MB) and, with the optimization mods on, the mods (about
   550 MB). Later runs reuse what was converted: keep the same work folder.
5. Install `output\mw3ds.cia` with FBI (see [Install it](#6-install-it)).

If it stops, the whole log is in `work\converter-log.txt`: attach it to a
[bug report](#testing-and-reporting). To build from source instead, follow the Quick start below.

## The converter on Linux and the Steam Deck (podman)

The same converter comes as a container image, with everything it needs inside: no Python, devkitPro or git. SteamOS
ships podman, so on a Steam Deck switch to Desktop Mode, open Konsole and run:

```sh
mkdir -p ~/mw3ds
podman run --rm -v ~/.local/share/Steam/steamapps/common/Morrowind:/morrowind:ro -v ~/mw3ds:/out \
    ghcr.io/3ds-conspiracy/mw3ds-converter --tribunal --bloodmoon
```

The CIA lands in `~/mw3ds`. Leave out `--tribunal` / `--bloodmoon` for a game without those add-ons, and add
`--old3ds` for an Old 3DS or 2DS. The first run downloads the image (about 80 MB, 330 MB unpacked), ffmpeg (about 30 MB) and the
optimization mods (about 550 MB; `--no-mods` skips them) and takes an hour or more; later runs reuse what was
converted in `~/mw3ds/work`, about 5 GB. `--help` lists the rest (the game's language, plugins).

Elsewhere on Linux, point the first `-v` at your own Morrowind folder (the one holding `Data Files`). Docker works the
same way: add `--user "$(id -u):$(id -g)"` so the files it writes are yours, and on Fedora and other SELinux systems add
`:z` to the `/out` mount.

## Quick start

### 1. Get the code

```sh
git clone --recurse-submodules https://github.com/3ds-conspiracy/mw3ds.git
```

`--recurse-submodules` also downloads the two optimization mods the builds use (see
[The optimization mods](#the-optimization-mods)). Already cloned without it? The build downloads them itself.

### 2. Install the prerequisites

You need a Windows PC (Linux and macOS can build too: the scripts call `make` directly there, with devkitPro
installed, `DEVKITPRO` set and `sudo dkp-pacman -S 3ds-dev 3ds-zlib`. A Linux build has been played on a New 3DS)
with:

- **Morrowind**, installed anywhere. Only the base game is used; Tribunal and Bloodmoon are not needed.
- **[Python](https://www.python.org/downloads/) 3.10 or newer.** Tick "Add Python to PATH" in the installer.
- **[Git](https://git-scm.com/download/win).**
- **[MSYS2](https://www.msys2.org/)**, with devkitPro's pacman repository added to it. Follow
  [devkitPro's instructions](https://devkitpro.org/wiki/devkitPro_pacman); you only need the part that sets up the
  repository, the setup script below installs the packages.
- **[Azahar](https://azahar-emu.org/)** (optional), the portable version, only to run the automated tests.
- About 5 GB of free disk space (the mods, the converted game data, the copy staged for the CIA and the CIA itself).

### 3. Tell the tools where things are

Copy `.env.example` to `.env` in the repository folder and fill in your paths:

```ini
MW3DS_MORROWIND_DIR=C:\Games\Morrowind
MW3DS_MSYS2_DIR=C:\msys64
MW3DS_CTR_TOOLS_DIR=
MW3DS_EMU_DIR=C:\Tools\azahar
```

| Setting | What to put |
|---|---|
| `MW3DS_MORROWIND_DIR` | Your Morrowind folder: the one with `Morrowind.ini` and `Data Files` in it |
| `MW3DS_MSYS2_DIR` | Your MSYS2 folder |
| `MW3DS_CTR_TOOLS_DIR` | Leave blank: the setup script downloads `makerom` and fills this in |
| `MW3DS_EMU_DIR` | Your Azahar folder (only needed for the emulator and tests) |

`.env` stays on your machine; git ignores it.

### 4. Install the rest

```sh
python tools/build/setup.py
```

This installs the Python packages, the 3DS toolchain and `makerom`, then checks everything. If something is missing it
says what and where to get it. Run it again with `--check` any time to see the state without installing anything.

### 5. Build

| Console | Command | Writes |
|---|---|---|
| **Old 3DS / Old 2DS** | `python tools/build/build_cia_mods.py --old3ds` | `build\mw3ds-o3ds.cia` |
| New 3DS / New 2DS | `python tools/build/build_cia_mods.py` | `build\mw3ds.cia` |

Each converts the game data from your Morrowind install (with the mods laid over it), builds the engine and packs both
into the CIA (about 1 GB). The first run takes an hour or more; later runs reuse what is already converted.

After changing only the engine code (a new commit), skip the conversion:

```sh
git pull --recurse-submodules
python tools/build/build_cia_mods.py --old3ds --skip-convert
```

The Old 3DS data (textures at most 128 texels, a quarter of the memory) goes to `out\o3ds`, the New 3DS data to
`out\data`: you can have both. Install the one for your console: an Old 3DS given the New 3DS's CIA shows a screen
that says so and quits, rather than stopping on a black screen at the first load. (Data converted before 12 October
2026 carries no mark of its console, so the check lets it through.)

### 6. Install it

Copy the CIA to your 3DS's SD card and install it with your CIA installer (FBI, for example). It shows up on the HOME
Menu as **MW3DS**. Saves and settings live in `sdmc:/3ds/mw3ds/`.

## On an Old 3DS

The engine finds out which console it is on and, on an Old 3DS, takes a lighter profile:

- **Memory**: the 64 MB mode, split with more for the game and less for graphics; fewer far-away objects kept in memory.
- **Frame rate**: a steady 20 outdoors and 30 indoors, 20 where a room is too heavy (both even shares of the screen's
  60 Hz, so no judder).
- **View**: a shorter view outdoors, the fog closing in further whenever frames come slower than 20 a second; distant
  land still reaches the horizon. People are drawn less far and animated less often in the distance.
- **3D** follows the 3D slider, as on a New 3DS (there is no Options setting). It costs a second picture each frame:
  keep the slider down for the best frame rate.

### Controls

An Old 3DS has no C-stick and no ZL / ZR (without the Circle Pad Pro), so its default layout is **GoldenEye**: one stick
that walks and turns, on the Xbox layout's buttons, and a button that stops you to aim. The D-pad strafes.

| Button | GoldenEye (Old 3DS default) |
|---|---|
| Circle Pad | Walk forward / back, turn left / right |
| A (hold), ZL / ZR (hold) | Aim: stand still, the Circle Pad aims (up and down too) |
| A (tap) | Activate: talk, open, take |
| X | Ready / put away the weapon |
| R (hold, release) | Wind up, strike (the Circle Pad keeps walking) |
| L | Cast the selected spell |
| Y | Jump |
| B | Sneak on / off |
| SELECT | First / third person |
| D-Pad | Left / right: strafe; up / down: look |
| START | Menu: save, rest, options |
| Touch screen | Inventory, magic, map, stats, quick keys |

**Gyro aim** (Options > Controls): turning the 3DS turns your view, for fine aiming; off, 0.5x, 1x or 2x, kept between
sessions. Choosing a speed calibrates it: lay the 3DS down for 2 seconds (it also tries once as the game starts). The
same tab sets one look speed for the sticks, the D-pad and GoldenEye aiming, the GoldenEye turn while walking, and the
sticks' dead zone (raise it if a stick keeps walking or turning after you let go). Layout shows the buttons and switches
between GoldenEye, Xbox and classic (Y).

## Testing and reporting

What helps most from a play test on an Old 3DS:

- **Smoothness**: where it drops below a steady 30 (towns? the wild?), and whether 20 fps places feel playable. The
  bottom screen shows the frame rate.
- **Memory**: missing textures (grey or untextured objects), people or places that don't appear, a load that stops,
  "out of memory" messages.
- **Controls**: whether the GoldenEye layout and gyro aim feel right (does turning the console turn the view the
  right way?).
- **Crashes and freezes**: where you were and what you did just before.

Report them as [GitHub issues](../../issues) and say it was an Old 3DS. Attach `sdmc:/3ds/mw3ds/log.txt` (and
`crash.txt` / `watchdog.txt` there if they exist). See [`CONTRIBUTING.md`](CONTRIBUTING.md); never attach game files or
builds.

## The optimization mods

The builds above lay two community mods over your game files: the
[Morrowind Optimization Patch](https://github.com/mop-org/morrowind-optimization-patch) and
[Project Atlas](https://github.com/MelchiorDahrk/Project-Atlas) (fewer, bigger meshes: fewer draws per frame). They are
**git submodules** in `mods\`, each pinned to a commit; `git submodule update --init` downloads them (about 550 MB),
and the build runs it for you. Your Morrowind install is not modified. A submodule is only a link to the other
repository, so nothing from the mods is part of this one; they keep their own licenses (Atlas: Unlicense; the patch
repository has no license file).

Without the mods: `python tools/build/build_cia.py` (or `--old3ds`). It writes the same folders, so switching means
converting again.

## Plugins and other languages

**Plugins.** Put the `.esp` files in `Data Files` as usual (with their meshes and textures) and list them in `.env`,
in load order: `MW3DS_PLUGINS=Siege at Firemoth.esp;master_index.esp`. The converter merges them with
`Morrowind.esm` the way OpenMW does. Plugins that need Tribunal or Bloodmoon, MWSE, MGE XE or OpenMW Lua are not
supported. At the end of a conversion each plugin gets a line listing any script functions the engine does not run
yet, so you can see why a mod only half works.

**Translated Morrowind.** A German, French, Polish, Russian ... `Morrowind.esm` converts like the English one: set
`MW3DS_ENCODING` for its alphabet and convert. See [LANGUAGES.md](LANGUAGES.md) for the steps and what changes
language.

## Running the tests

The tests play the game automatically and check the results. Each test is a text file in `tools\test\cases\` listing
inputs and checks (go to a place, talk about a topic, expect a journal entry, ...).

```sh
python tools/test/run_test.py mq-ch1 --start "Seyda Neen"                                    # in Azahar
python tools/test/run_test.py perf-balmora --old3ds --data out/o3ds --start "Seyda Neen"      # Azahar as an Old 3DS
python tools/test/run_test.py soak-o3ds --old3ds --data out/o3ds --start "Seyda Neen" --wait 1500
```

`--old3ds` runs Azahar as an Old 3DS (the engine then keeps to the 64 MB mode's memory, as the console would). It checks
memory and behaviour; frame times at the Old 3DS's 268 MHz need the console itself. `tools\test\perf.py <log>` sums up a
run's per-second performance lines. The game's log is at `<Azahar folder>\user\sdmc\3ds\mw3ds\log.txt`.

## Troubleshooting

- **"... is not set"**: a path is missing from `.env`. Fill it in and run again.
- **Something else missing**: run `python tools/build/setup.py --check` to see what.
- **"git submodule update failed"**: no network, or git can't reach GitHub. Try again, or build without the mods.
- **"missing sounds" during the build**: a few sound files the game refers to were not found. The build carries on
  without them; those sounds are silent.
- **Need to start the data over**: delete `out\o3ds` (or `out\data`) and build again.

## How it works

```
Your Morrowind install ──▶ tools\convert (converters, mods laid over) ──▶ out\o3ds ──▶ packed into the CIA ──▶ MW3DS on the 3DS
```

| Folder | What is in it |
|---|---|
| `source\`, `include\` | The engine: world and rendering, combat, magic, dialogue, the MWScript interpreter, UI, saves, and the test harness |
| `tools\convert\` | The converters (Python): Morrowind's files to the engine's data; `level.py` runs them all |
| `tools\build\` | Setup, build and packaging scripts (Python), the CIA's banner and RSF |
| `tools\test\` | The test runners, quest sweeps, spec test generators and data checks |
| `tools\test\cases\` | The automated tests |
| `mods\` | The optimization mods (git submodules) |
| `spec\` | The game's formulas (spell cost, damage, persuasion, alchemy, ...) as documented by OpenMW |

## Status

Under development. Builds are played on a real New 3DS and a real Old 2DS and fixed from what is seen there. The main quest's 15 chapters pass their automated tests. The
engine follows OpenMW's rules where they are known.

## Contributing

Bug reports and suggestions are welcome as [GitHub issues](../../issues). **Pull requests are not accepted** and will
be closed. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for what to include. Never attach Morrowind files, converted data or
builds.

## License

The code is licensed under the GNU General Public License v3.0 or later ([`LICENSE`](LICENSE)). `source/extern/cjson/cJSON.c` and
`source/extern/cjson/cJSON.h` are [cJSON](https://github.com/DaveGamble/cJSON), MIT licensed. The specs in `spec/` describe
mechanics as documented by the [OpenMW](https://openmw.org/) project and the community.

The license covers this repository's code only. It grants no rights to Morrowind or any other Bethesda content.
