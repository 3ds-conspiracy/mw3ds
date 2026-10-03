# Runs sweep batches (and tests) across several Azahar copies at once: one worker process per emulator
# (tools\test\sweep-worker.ps1), each taking the next unclaimed item until none are left. Each batch's check goes
# to build\par\<name>.result; they're appended to build\<prefix>-results.txt in batch order at the end.
#   tools\test\sweep-par.ps1 -ItemsFile build\par-items.txt [-Emus a,b,c,d] [-Data out\world]
#   items (one a line): <prefix>-<n> (a sweep batch, checked by questrun.py) or test:<name> (tools\test\cases)
# Progress: build\par\progress.txt
param([string]$ItemsFile, [string]$Emus = '',
      [string]$Data = 'out\world', [string]$Start = 'Seyda Neen')
. (Join-Path $PSScriptRoot '..\build\env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Emus) { $Emus = Get-MwEnv 'MW3DS_EMU_DIRS' 'comma separated Azahar folders' }
$par = Join-Path $root 'build\par'
New-Item -ItemType Directory -Force $par, (Join-Path $root 'build\qrun-logs') | Out-Null
Remove-Item (Join-Path $par '*.claim'), (Join-Path $par 'progress.txt') -Force -ErrorAction SilentlyContinue
$items = Get-Content (Resolve-Path $ItemsFile) | Where-Object { $_.Trim() }
Set-Content (Join-Path $par 'queue.txt') $items -Encoding ascii
# the binary as it is now (rebuilding meanwhile doesn't change what the batches run)
$app = Join-Path $root 'build\par-app.3dsx'
Copy-Item (Join-Path $root 'mw3ds.3dsx') $app -Force
$procs = foreach ($e in ($Emus -split ',')) {
    Start-Process powershell -ArgumentList '-NoProfile', '-File', (Join-Path $PSScriptRoot 'sweep-worker.ps1'), '-Emu', "`"$e`"",
        '-App', "`"$app`"", '-Data', $Data, '-Start', "`"$Start`"" -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 20          # staggered: four emulators booting at once load the disk together
}
$procs | Wait-Process
foreach ($prefix in ($items | Where-Object { $_ -notlike 'test:*' } | ForEach-Object { $_ -replace '-\d+$', '' } | Sort-Object -Unique)) {
    Get-ChildItem $par -Filter "$prefix-*.result" | Sort-Object { [int]($_.BaseName -replace '\D', '') } |
        ForEach-Object { Get-Content $_.FullName } | Add-Content (Join-Path $root "build\$prefix-results.txt")
}
"sweep done"
