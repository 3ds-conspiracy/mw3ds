# Opens mw3ds.3dsx in Azahar for interactive play (no test hooks).
#   tools\play-emu.ps1 -Start "Seyda Neen"   start in that cell, skipping character creation
#   tools\play-emu.ps1 -NewGame              delete the save (sdmc:/3ds/mw3ds/save.json) and start over
# Otherwise the game continues from the save, if there is one.
param([string]$Start, [switch]$NewGame)
$root  = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'env.ps1')
$azDir = (Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder')
$sd    = Join-Path $azDir 'user\sdmc\3ds\mw3ds'
$data  = Join-Path $root 'out\data'

Get-Process azahar -ErrorAction SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force $sd | Out-Null
Remove-Item (Join-Path $sd 'autoshot'), (Join-Path $sd 'autocam.txt'), (Join-Path $sd 'autoinput.txt') -Force -ErrorAction SilentlyContinue
if ($NewGame) { Remove-Item (Join-Path $sd 'save.json') -Force -ErrorAction SilentlyContinue }
$startFile = Join-Path $sd 'start.txt'
if ($Start) { Set-Content -Path $startFile -Value $Start -Encoding ascii } else { Remove-Item $startFile -Force -ErrorAction SilentlyContinue }
if (Test-Path $data) { robocopy $data (Join-Path $sd 'data') /MIR /NJH /NJS /NFL /NDL /NP | Out-Null }
Start-Process -FilePath (Join-Path $azDir 'azahar.exe') -ArgumentList "`"$(Join-Path $root 'mw3ds.3dsx')`""
