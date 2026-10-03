# Makes a bundled starting-point save: starts the game in a cell (character creation skipped),
# lets it autosave, and copies the save into out\data\saves\ so it ships in the RomFS and shows
# on the title / Saves screens. Re-run after re-converting the level (saves point at refs by index).
#   tools\make-save.ps1 -Cell Balmora -Name balmora -Title "Balmora"
#   tools\make-save.ps1 -Cell "Vivec, Foreign Quarter" -Name vivec -Title Vivec -Data out\world -Gold 5000
param([string]$Cell = "Balmora", [string]$Name = "balmora", [string]$Title = "Balmora", [int]$Wait = 150,
      [string]$Data = 'out\data', [int]$Gold = 0)

$root  = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'env.ps1')
$azDir = (Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder')
$sd    = Join-Path $azDir 'user\sdmc\3ds\mw3ds'
$outDir = Join-Path (Join-Path $root $Data) 'saves'

Remove-Item (Join-Path $sd 'save.json'), (Join-Path $sd 'save.json.meta') -Force -ErrorAction SilentlyContinue
$inputs = @("3 0 0 0")
if ($Gold -gt 0) { $inputs += "0.3 0 0 0 0 GIVE:gold_001:$Gold"; $inputs += "1 0 0 0" }
& (Join-Path $PSScriptRoot 'run-emu.ps1') -Start $Cell -Data $Data -Inputs $inputs -Wait $Wait | Out-Null

$save = Join-Path $sd 'save.json'
if (-not (Test-Path $save)) { "no save written - see the log"; exit 1 }
New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item $save (Join-Path $outDir "$Name.json") -Force
# name \t place \t level \t time: time 0 = no date shown
Set-Content -Path (Join-Path $outDir "$Name.json.meta") -Value "$Title`t$Cell`t1`t0" -Encoding ascii
$size = [math]::Round((Get-Item (Join-Path $outDir "$Name.json")).Length / 1KB)
"bundled save: $Data\saves\$Name.json ($size KB)"
Select-String -Path (Join-Path $sd 'log.txt') -Pattern 'save:|load ' | ForEach-Object { $_.Line }
