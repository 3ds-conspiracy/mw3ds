# MW3DS: Morrowind for the New 3DS

Play *The Elder Scrolls III: Morrowind* on a **New Nintendo 3DS**. MW3DS is a from-scratch engine written in C++ for the
3DS. It runs the base game on real hardware and in the [Azahar](https://azahar-emu.org/) emulator.

> [!IMPORTANT]
> **For testing and research only. You need your own legally obtained copy of Morrowind.**
> This repository contains no Morrowind content. The tools convert the game files from your own install, on your own
> machine. The converted data and any build made from it contain copyrighted material: keep them to yourself and never
> share or upload them. MW3DS is an unofficial fan project, not affiliated with or endorsed by Bethesda Softworks or
> ZeniMax Media.

## Quick start

### 1. Install the prerequisites

You need a Windows PC with:

- **Morrowind**, installed anywhere. Only the base game is used; Tribunal and Bloodmoon are not needed.
- **[Python](https://www.python.org/downloads/) 3.10 or newer.** Tick "Add Python to PATH" in the installer.
- **[MSYS2](https://www.msys2.org/)**, with devkitPro's pacman repository added to it. Follow
  [devkitPro's instructions](https://devkitpro.org/wiki/devkitPro_pacman); you only need the part that sets up the
  repository, the setup script below installs the packages.
- **[Azahar](https://azahar-emu.org/)** (optional), the portable version, only to run the automated tests.
- A few GB of free disk space. The converted game data, the copy staged for the CIA, and the CIA itself are about
  1 GB each.

### 2. Tell the tools where things are

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

### 3. Install the rest

```powershell
powershell -File tools\build\setup.ps1
```

This installs the Python packages, the 3DS toolchain and `makerom`, then checks everything. If something is missing
it says what and where to get it. Run it again with `-Check` any time to see the state without installing anything.

### 4. Build

```powershell
powershell -File tools\build\build-cia.ps1
```

This converts the game data from your Morrowind install, builds the engine and packs both into **`build\mw3ds.cia`**
(about 1.1 GB). The first run takes around 30 minutes; later runs are faster.

After changing only the engine code, skip the conversion:

```powershell
powershell -File tools\build\build-cia.ps1 -SkipConvert
```

## Running the tests

The tests play the game automatically in Azahar and check the results. Each test is a text file in `tools\test\cases\`
listing inputs and checks (go to a place, talk about a topic, expect a journal entry, ...).

```powershell
powershell -File tools\test\run-test.ps1 mq-ch1 -Start "Seyda Neen"   # one test; prints RESULT: PASS or FAIL
powershell -File tools\test\run-suite.ps1 -Data out\data              # the 15 main quest chapters, one after another
```

They cover the main quest, mechanics such as escorts, strongholds and vampirism, quest and dialogue sweeps, and
formula tests generated from OpenMW's rules. The game's log is at `<Azahar folder>\user\sdmc\3ds\mw3ds\log.txt`.

## Troubleshooting

- **"... is not set"**: a path is missing from `.env`. Fill it in and run again.
- **Something else missing**: run `powershell -File tools\build\setup.ps1 -Check` to see what.
- **"missing sounds" during the build**: a few sound files the game refers to were not found. The build carries on
  without them; those sounds are silent.
- **Need to start the data over**: delete the `out` folder and run `tools\build\build-cia.ps1` again.

## How it works

```
Your Morrowind install ──▶ tools\convert (converters) ──▶ out\data ──▶ packed into the CIA ──▶ MW3DS engine on the 3DS
```

| Folder | What is in it |
|---|---|
| `source\`, `include\` | The engine: world and rendering, combat, magic, dialogue, the MWScript interpreter, UI, saves, and the test harness |
| `tools\convert\` | The converters (Python): Morrowind's files to the engine's data; `level.py` runs them all |
| `tools\build\` | Setup, build and packaging scripts (PowerShell), the CIA's banner and RSF |
| `tools\test\` | The test runners, quest sweeps, spec test generators and data checks |
| `tools\test\cases\` | The automated tests |
| `spec\` | The game's formulas (spell cost, damage, persuasion, alchemy, ...) as documented by OpenMW |

## Status

Under development. Builds are played on a real New 3DS and fixed from what is seen there. The main quest's 15 chapters
pass their automated tests. The engine follows OpenMW's rules where they are known.

## Contributing

Bug reports and suggestions are welcome as [GitHub issues](../../issues). **Pull requests are not accepted** and will
be closed. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for what to include. Never attach Morrowind files, converted data or
builds.

## License

The code is licensed under the GNU General Public License v3.0 or later ([`LICENSE`](LICENSE)). `source/cJSON.c` and
`include/cJSON.h` are [cJSON](https://github.com/DaveGamble/cJSON), MIT licensed. The specs in `spec/` describe
mechanics as documented by the [OpenMW](https://openmw.org/) project and the community.

The license covers this repository's code only. It grants no rights to Morrowind or any other Bethesda content.
