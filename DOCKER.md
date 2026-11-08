# Converting with Docker

The MW3DS converter turns your own Morrowind into a CIA for a New 3DS or Old 3DS. It also comes as a container image,
with everything it needs inside: no Python, no devkitPro, no git, and no program from this project on your computer.
The container only sees the two folders you give it: your Morrowind folder (read only) and an output folder. Nothing
from Morrowind is in the image: it reads your copy.

On a Steam Deck use [STEAM-DECK.md](STEAM-DECK.md) (podman is already there). This page is for Windows and other Linux
machines with Docker.

## What you need

- **Docker.** On Windows, [Docker Desktop](https://www.docker.com/products/docker-desktop/) (it runs Linux containers
  through WSL 2, which its installer sets up). On Linux, Docker Engine or podman.
- **Morrowind installed** (Steam, GOG or disc). It only has to be there; it never has to run. The Game of the Year
  edition comes with Tribunal and Bloodmoon, and the converter can put both in.
- **About 6 GB free** on the drive that holds the output folder (the converted game, its caches and the CIA).
- **An hour or more.** The first run downloads the image (about 80 MB), ffmpeg (about 30 MB) and the optimization mods
  (about 550 MB). Later runs are much faster.

## 1. Find your Morrowind folder

It is the folder that holds `Morrowind.exe` and `Data Files`. For Steam on Windows it is usually:

```
C:\Program Files (x86)\Steam\steamapps\common\Morrowind
```

In Steam, right-click Morrowind, then **Manage**, **Browse local files** shows where.

## 2. Convert

Make a folder for the result, such as `C:\MW3DS-out`, then open **PowerShell** and run (one line):

```powershell
docker run --rm -v "C:\Program Files (x86)\Steam\steamapps\common\Morrowind:/morrowind:ro" -v "C:\MW3DS-out:/out" ghcr.io/3ds-conspiracy/mw3ds-converter:0.2.8 --tribunal --bloodmoon
```

On Linux, in a terminal (the `--user` part makes the files it writes yours):

```sh
mkdir -p ~/mw3ds
docker run --rm --user "$(id -u):$(id -g)" -v ~/.local/share/Steam/steamapps/common/Morrowind:/morrowind:ro -v ~/mw3ds:/out \
    ghcr.io/3ds-conspiracy/mw3ds-converter:0.2.8 --tribunal --bloodmoon 2>&1 | tee ~/mw3ds/converter-log.txt
```

(With podman, say `podman` for `docker` and leave out `--user`. On Fedora and other SELinux systems add `:z` to the
`/out` mount.)

Change the command to fit:

- Another Morrowind folder: change the part before `:/morrowind`.
- A newer version: change `0.2.8` to the newest one on the [Releases](https://github.com/3ds-conspiracy/mw3ds/releases)
  page.
- An **Old 3DS or 2DS**: add `--old3ds` (smaller textures).
- No Tribunal or Bloodmoon (not the Game of the Year edition): leave out `--tribunal` / `--bloodmoon`.
- Without the optimization mods: add `--no-mods` (they make the game run faster on the 3DS; leave them on unless you
  have a reason).
- A Polish, Czech or Hungarian game: add `--encoding win1250`; Russian or Ukrainian: `--encoding win1251`.
- `--help` in place of the options lists them all.

It ends with `done: ... is in your output folder`. The CIA is in the output folder:
`mw3ds-tribunal-bloodmoon.cia` with both add-ons, `mw3ds-o3ds-tribunal-bloodmoon.cia` for an Old 3DS, `mw3ds.cia`
with neither.

On Windows the folders you share with the container are read through Docker Desktop's file sharing, which is slower than
a normal disk: expect the run to take longer than the times above.

## 3. Put it on the 3DS

Copy the CIA to the `cias` folder of the 3DS's SD card and install it with FBI. Two ways:

- **The card:** put the 3DS's SD card in your computer (a New 3DS takes a microSD card; an Old 3DS's full-size SD card
  may need a card reader) and copy the file over.
- **Over Wi-Fi:** start ftpd on the 3DS and send the file to the address ftpd shows (`ftp://<address>:5000/cias/`) with
  an FTP client.

## If it stops

On Linux the whole run is in `~/mw3ds/converter-log.txt`. On Windows, copy the text from the PowerShell window or run
the command with `2>&1 | Tee-Object C:\MW3DS-out\converter-log.txt` on the end. Attach the log to a
[bug report](https://github.com/3ds-conspiracy/mw3ds/issues). Run the same command again to carry on: what was already
converted is kept in the `work` folder inside your output folder. To start clean, delete that folder.
