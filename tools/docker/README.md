# Building the CIA in Docker

Needs Docker Desktop (BuildKit is on by default) and your own Morrowind install. Put the install folder in
`.env` (copy `.env.example`), the same file the PowerShell build uses:

```
MW3DS_MORROWIND_DIR=C:\Program Files\GOG Galaxy\Games\Morrowind
```

Then:

```
powershell -File tools\docker\build.ps1
```

`output\mw3ds.cia` is the result. The script passes your install to the build as a named context
(`--build-context morrowind=<that folder>`), which the Dockerfile copies `Data Files` from. Docker's layer
cache keeps finished steps, so a rerun only redoes what changed; `docker builder prune` starts clean.

If it fails, the last `decoding` line in the output names the sound file it stopped on.
