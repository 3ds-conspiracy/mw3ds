# Builds build\mw3ds.cia from scratch: the program, the game data converted from your Morrowind install, and the CIA.
#   tools\build\build-cia.ps1                  whole island (about 30 min the first time, minutes after that)
#   tools\build\build-cia.ps1 -Small           only the area around Balmora (quick, for a first try)
#   tools\build\build-cia.ps1 -SkipConvert     reuse the data already in out\data
#   tools\build\build-cia.ps1 -Old3ds          for an Old 3DS: textures at most 128 texels (a quarter of the memory), data in
#                                         out\o3ds, build\mw3ds-o3ds.cia (the program is the same: it finds the console itself)
# Needs .env filled in (see .env.example): MW3DS_MORROWIND_DIR, MW3DS_MSYS2_DIR, MW3DS_CTR_TOOLS_DIR.
param([switch]$Small, [switch]$SkipConvert, [switch]$Old3ds)
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

$data = if ($Old3ds) { 'out\o3ds' } else { 'out\data' }
$cia = if ($Old3ds) { 'build\mw3ds-o3ds.cia' } else { 'build\mw3ds.cia' }
$convertArgs = @()
if (-not $Small) { $convertArgs += '--world' }
if ($Old3ds) { $convertArgs += @('--max-tex', '128'); $env:MW3DS_OUT = 'out/o3ds' }

# A CIA left by an earlier failed build must not be mistaken for this one
Remove-Item (Join-Path $root $cia) -ErrorAction SilentlyContinue

# 1. Convert the game data (make-cia.ps1 builds the program itself)
if (-not $SkipConvert) {
    "== converting game data (python $(python --version 2>&1))"
    python tools\convert\level.py @convertArgs
    if ($LASTEXITCODE -ne 0) { throw "level.py failed (exit code $LASTEXITCODE). Send the whole output: the last 'decoding' line names the sound file it stopped on." }
} elseif (-not (Test-Path (Join-Path $root $data))) {
    throw "$data does not exist: run without -SkipConvert first"
}

# 2. Program + CIA
"== building the program and the CIA"
& (Join-Path $PSScriptRoot 'make-cia.ps1') -Data $data -Out $cia
if ($LASTEXITCODE -ne 0) { throw 'CIA build failed: see the make / makerom output just above this error' }
"Built $cia."
