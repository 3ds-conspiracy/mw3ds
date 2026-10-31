# Converting on a Steam Deck

The MW3DS converter turns your own Morrowind into a CIA for a New 3DS or Old 3DS. On a Steam Deck it runs as a
container image with podman, which SteamOS already has: nothing to install, no Python, no devkitPro. Nothing from
Morrowind is in the image: it reads your copy.

## What you need

- **Morrowind installed through Steam on the Deck.** It only has to be downloaded; it never has to run. The Game of
  the Year edition comes with Tribunal and Bloodmoon, and the converter can put both in.
- **About 6 GB free** in your home folder (the converted game, its caches and the CIA).
- **An hour or more**, plugged in. The first run downloads the image (about 80 MB), ffmpeg (about 30 MB) and the
  optimization mods (about 550 MB). Later runs are much faster.

## 1. Open a terminal

Hold the power button, pick **Switch to Desktop**, then open **Konsole** from the application menu (System).

## 2. Find your Morrowind folder

It is usually here:

```sh
ls ~/.local/share/Steam/steamapps/common/Morrowind
```

If that lists `Data Files` and `Morrowind.exe`, that is the folder. If Morrowind is on the microSD card, it is under
`/run/media/` instead: in Steam, right-click Morrowind, then **Manage**, **Browse local files** shows where.

## 3. Convert

```sh
mkdir -p ~/mw3ds
podman run --rm -v ~/.local/share/Steam/steamapps/common/Morrowind:/morrowind:ro -v ~/mw3ds:/out \
    ghcr.io/3ds-conspiracy/mw3ds-converter:0.2.2 --tribunal --bloodmoon 2>&1 | tee ~/mw3ds/converter-log.txt
```

Change the command to fit:

- Another Morrowind folder: change the part before `:/morrowind`.
- A newer version: change `0.2.2` to the newest one on the [Releases](https://github.com/3ds-conspiracy/mw3ds/releases)
  page.
- An **Old 3DS or 2DS**: add `--old3ds` (smaller textures).
- No Tribunal or Bloodmoon (not the Game of the Year edition): leave out `--tribunal` / `--bloodmoon`.
- Without the optimization mods: add `--no-mods` (they make the game run faster on the 3DS; leave them on unless you
  have a reason).
- A Polish, Czech or Hungarian game: add `--encoding win1250`; Russian or Ukrainian: `--encoding win1251`.
- `--help` in place of the options lists them all.

It ends with `done: ... is in your output folder`. The CIA is in `~/mw3ds`:
`mw3ds-tribunal-bloodmoon.cia` with both add-ons, `mw3ds-o3ds-tribunal-bloodmoon.cia` for an Old 3DS, `mw3ds.cia`
with neither.

## 4. Put it on the 3DS

Copy the CIA to the `cias` folder of the 3DS's SD card and install it with FBI. Two ways:

- **The card:** a New 3DS takes a microSD card, which fits the Deck's own slot (open it in Dolphin, the file
  manager). An Old 3DS's full-size SD card needs a USB card reader.
- **Over Wi-Fi:** start ftpd on the 3DS and copy the file with Dolphin to the address ftpd shows
  (`ftp://<address>:5000/cias/`).

## If it stops

The whole run is in `~/mw3ds/converter-log.txt`: attach it to a
[bug report](https://github.com/3ds-conspiracy/mw3ds/issues). Run the same command again to carry on: what was
already converted is kept in `~/mw3ds/work`. To start clean, delete that folder.

The same command works on any Linux with podman. With Docker, add `--user "$(id -u):$(id -g)"` after `run` so the
files it writes are yours.
