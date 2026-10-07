# Builds build\mw3ds.cia from scratch: the program, the game data converted from your Morrowind install, and the CIA.
#   tools\build\build-cia.ps1                  whole island (about 30 min the first time, minutes after that)
#   tools\build\build-cia.ps1 -Small           only the area around Balmora (quick, for a first try)
#   tools\build\build-cia.ps1 -SkipConvert     reuse the data already in out\data
# Needs .env filled in (see .env.example): MW3DS_MORROWIND_DIR, MW3DS_MSYS2_DIR, MW3DS_CTR_TOOLS_DIR.
param([switch]$Small, [switch]$SkipConvert)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root

# Check the setup before the long steps
$morrowind = Get-MwEnv 'MW3DS_MORROWIND_DIR' 'the folder of your Morrowind install'
$null = Get-MwEnv 'MW3DS_MSYS2_DIR' 'your MSYS2 folder'
$ctr = Get-MwEnv 'MW3DS_CTR_TOOLS_DIR' 'the folder with makerom.exe'
if (-not (Test-Path (Join-Path $morrowind 'Data Files\Morrowind.esm'))) { throw "Morrowind.esm not found under $morrowind\Data Files" }
if (-not (Test-Path (Join-Path $ctr 'makerom.exe'))) { throw "makerom.exe not found in $ctr" }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'python not found on PATH' }

# A CIA left by an earlier failed build must not be mistaken for this one
Remove-Item (Join-Path $root 'build\mw3ds.cia') -ErrorAction SilentlyContinue

# 1. Convert the game data (make-cia.ps1 builds the program itself)
if (-not $SkipConvert) {
    "== converting game data (python $(python --version 2>&1))"
    if ($Small) { python tools\convert\level.py } else { python tools\convert\level.py --world }
    if ($LASTEXITCODE -ne 0) { throw "level.py failed (exit code $LASTEXITCODE). Send the whole output: the last 'decoding' line names the sound file it stopped on." }
} elseif (-not (Test-Path (Join-Path $root 'out\data'))) {
    throw 'out\data does not exist: run without -SkipConvert first'
}

# 2. Program + CIA
"== building the program and the CIA"
& (Join-Path $PSScriptRoot 'make-cia.ps1')
if ($LASTEXITCODE -ne 0) { throw 'CIA build failed: see the make / makerom output just above this error' }
"Built build\mw3ds.cia."
