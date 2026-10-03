# One emulator's share of a parallel sweep (tools\sweep-par.ps1 starts one per Azahar copy): takes the next
# unclaimed item of build\par\queue.txt until none are left.
param([string]$Emu, [string]$App, [string]$Data = 'out\world', [string]$Start = 'Seyda Neen')
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$par = Join-Path $root 'build\par'
$log = Join-Path $Emu 'user\sdmc\3ds\mw3ds\log.txt'
foreach ($it in (Get-Content (Join-Path $par 'queue.txt'))) {
    # claim it (creating the claim file fails if another emulator got there first)
    # (named without the ':' of test:<name>, which a Windows file name can't hold)
    try { New-Item -ItemType File -Path (Join-Path $par "$($it -replace ':', '_').claim") -ErrorAction Stop | Out-Null } catch { continue }
    if ($it -like 'test:*') {
        $t = $it.Substring(5)
        & (Join-Path $PSScriptRoot 'run-test.ps1') $t -Data $Data -Start $Start -Wait 1500 -App $App -Emu $Emu `
            -Pattern 'expect: FAIL|drive: FAIL|monitor:|refused|not supported' | Out-File (Join-Path $par "$t.result") -Encoding utf8
        Copy-Item $log (Join-Path $par "$t.log") -Force
    } else {
        & (Join-Path $PSScriptRoot 'run-test.ps1') $it -Data $Data -Start $Start -Wait 2700 -Pattern 'zzzz' -App $App -Emu $Emu | Out-Null
        Copy-Item $log (Join-Path $root "build\qrun-logs\$it.log") -Force
        Copy-Item (Join-Path $Emu 'user\log\azahar_log.txt') (Join-Path $root "build\qrun-logs\$it.emu.log") -Force -ErrorAction SilentlyContinue
        python (Join-Path $PSScriptRoot 'questrun.py') --check $it --log (Join-Path $root "build\qrun-logs\$it.log") |
            Out-File (Join-Path $par "$it.result") -Encoding utf8
    }
    Add-Content (Join-Path $par 'progress.txt') "$(Get-Date -Format HH:mm) $(Split-Path $Emu -Leaf): $it done"
}
