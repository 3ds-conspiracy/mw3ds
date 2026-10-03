# Runs the side-quest sweep batches (tools/test/questrun.py) one after another on the island data:
#   tools\test\qrun-all.ps1 [-From 1] [-To 19]    results -> build\qrun-results.txt, logs -> build\qrun-logs
param([int]$From = 1, [int]$To = 19, [string]$Prefix = 'qrun')
. (Join-Path $PSScriptRoot '..\build\env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$logs = Join-Path $root 'build\qrun-logs'
New-Item -ItemType Directory -Force $logs | Out-Null
# the binary as it is now: rebuilding during the sweep must not change what the batches run
$app = Join-Path $root 'build\qrun-app.3dsx'
Copy-Item (Join-Path $root 'mw3ds.3dsx') $app -Force
$log = Join-Path (Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder') 'user\sdmc\3ds\mw3ds\log.txt'
for ($k = $From; $k -le $To; $k++) {
    & (Join-Path $PSScriptRoot 'run-test.ps1') "$Prefix-$k" -Data out\world -Start 'Seyda Neen' -Wait 2700 -Pattern 'zzzz' -App $app | Out-Null
    Copy-Item $log (Join-Path $logs "$Prefix-$k.log") -Force
    Copy-Item (Join-Path (Split-Path (Split-Path (Split-Path (Split-Path $log)))) 'log\azahar_log.txt') (Join-Path $logs "$Prefix-$k.emu.log") -Force
    python (Join-Path $PSScriptRoot 'questrun.py') --check "$Prefix-$k" | Add-Content (Join-Path $root "build\$Prefix-results.txt")
}
