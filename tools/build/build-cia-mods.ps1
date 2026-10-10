# Same as build-cia.ps1 (converts your Morrowind install and builds the CIA), with the community optimization mods
# (Morrowind Optimization Patch, Project Atlas) laid over the game files: fewer, bigger batches per frame. The mods are
# git submodules of this repository (mods\, pinned to a commit each): they are downloaded the first time (needs git and
# a network, about 550 MB) by `git submodule update --init`, which this script runs. Your Morrowind install is never
# modified, and nothing from the mods is part of this repository (a submodule is only a link to the other repository).
#   tools\build\build-cia-mods.ps1                whole island, New 3DS (out\data, build\mw3ds.cia)
#   tools\build\build-cia-mods.ps1 -Old3ds        whole island, Old 3DS (out\o3ds, build\mw3ds-o3ds.cia: see build-cia.ps1)
#   tools\build\build-cia-mods.ps1 -Small         only the area around Balmora
#   tools\build\build-cia-mods.ps1 -SkipConvert   reuse the converted data (it must have been converted with the mods)
#   tools\build\build-cia-mods.ps1 -FetchOnly     just download the mods and print the overlay folders
# The plain build (build-cia.ps1) writes the same folders: switching between the two means converting again.
param([switch]$Small, [switch]$SkipConvert, [switch]$FetchOnly, [switch]$Old3ds)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root

# The folders laid over Data Files, in load order (a later one wins when two ship the same file)
$overlays = @(
    'mods\morrowind-optimization-patch\00 Core',
    'mods\project-atlas\00 Core',
    'mods\project-atlas\01 Textures - Vanilla'
)

"== the mods (git submodules)"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git not found on PATH' }
git submodule update --init --depth 1 mods/morrowind-optimization-patch mods/project-atlas
if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed (network? see the message above)' }
$dirs = foreach ($o in $overlays) {
    $p = Join-Path $root $o
    if (-not (Test-Path $p)) { throw "overlay folder '$o' is missing: is the submodule checked out?" }
    "   overlay: $o" | Out-Host
    $p
}
if ($FetchOnly) { return }

# The converter (tools\convert\mwfiles.py) lays these folders over Data Files; a real environment variable wins over .env
$env:MW3DS_OVERLAY_DIRS = $dirs -join ';'

$buildArgs = @()
if ($Small) { $buildArgs += '-Small' }
if ($SkipConvert) { $buildArgs += '-SkipConvert' }
if ($Old3ds) { $buildArgs += '-Old3ds' }
& (Join-Path $PSScriptRoot 'build-cia.ps1') @buildArgs
exit $LASTEXITCODE
