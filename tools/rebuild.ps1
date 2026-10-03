# Everything from a fresh pull to a QR code: converts the game data, remakes the bundled Balmora save,
# builds the CIA and serves it for FBI's Remote Install > Scan QR Code.
#   tools\rebuild.ps1                 (extra arguments go to level.py, e.g. -- --towns Balmora)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

git pull
if ($LASTEXITCODE -ne 0) { "git pull failed"; exit 1 }
python tools\level.py @args
if ($LASTEXITCODE -ne 0) { "level.py failed"; exit 1 }
& (Join-Path $PSScriptRoot 'make-save.ps1') -Cell Balmora
& (Join-Path $PSScriptRoot 'make-cia.ps1')
if ($LASTEXITCODE -ne 0) { "CIA build failed"; exit 1 }
& (Join-Path $PSScriptRoot 'serve-cia.ps1')
