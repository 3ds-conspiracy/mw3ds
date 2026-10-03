# Runs a scripted test from tools\tests in Azahar and prints its result lines (check: / journal / crime ...).
#   tools\run-test.ps1 fg-rathunt                 (starts in Balmora, the Balmora-area data)
#   tools\run-test.ps1 tg-bragor -Data out\world  (the whole island)
#   tools\run-test.ps1 mainquest -Start "Seyda Neen"
param([Parameter(Mandatory = $true)][string]$Test, [string]$Start = 'Balmora', [string]$Data = 'out\data', [int]$Wait = 300, [string]$App = '',
      [string]$Emu = '',
      [string]$Pattern = 'check:|not offered|failed|expelled|crime:|expect:|monitor:|drive: (FAIL|stuck|crosshair miss)|was fighting|script error')
. (Join-Path $PSScriptRoot 'env.ps1')
$root = Split-Path $PSScriptRoot -Parent
if (-not $Emu) { $Emu = Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder' }
$inputs = Get-Content (Join-Path $root "tools\tests\$Test.txt")
& (Join-Path $PSScriptRoot 'run-emu.ps1') -Start $Start -Data $Data -Inputs $inputs -Wait $Wait -App $App -Emu $Emu | Out-Null
$log = Join-Path $Emu 'user\sdmc\3ds\mw3ds\log.txt'
"== $Test"
Select-String -Path $log -Pattern $Pattern | ForEach-Object { $_.Line }
if (-not (Select-String -Path $log -Pattern 'autoinput: end' -Quiet)) { "(did not reach the end)" }
# Memory faults the emulator saw (reads / writes of unmapped memory: a crash on hardware), by code address
$emuLog = Join-Path $Emu 'user\log\azahar_log.txt'
$faults = Select-String -Path $emuLog -Pattern 'Unmapped\w+ @ \S+ at PC (0x[0-9A-Fa-f]+)|Unreachable code' -ErrorAction SilentlyContinue
if ($faults) {
    $pcs = $faults | ForEach-Object { if ($_.Matches[0].Groups[1].Success) { $_.Matches[0].Groups[1].Value } else { 'gpu-unreachable' } } | Group-Object | Sort-Object Count -Descending
    "emulator faults: " + (($pcs | Select-Object -First 5 | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ') + "  (tools\addr2line.ps1 <pc>)"
}

# Verdict: story (expectations, driver failures, monitors, faults, reaching the end) and, apart, navigation
# (the driver warped past a spot it couldn't walk: the story still ran, the route needs a look)
$pass = @(Select-String -Path $log -Pattern 'expect: PASS').Count
$fail = @(Select-String -Path $log -Pattern 'expect: FAIL').Count
$drive = @(Select-String -Path $log -Pattern 'drive: FAIL').Count
$warps = @(Select-String -Path $log -Pattern 'drive: stuck').Count
$misses = @(Select-String -Path $log -Pattern 'drive: crosshair miss').Count
$mon = @(Select-String -Path $log -Pattern 'monitor: ').Count
$ended = Select-String -Path $log -Pattern 'autoinput: end' -Quiet
$bad = $fail + $drive + $mon + $(if ($faults) { 1 } else { 0 }) + $(if ($ended) { 0 } else { 1 })
"RESULT: " + $(if ($bad -eq 0) { 'PASS' } else { 'FAIL' }) + " ($pass expectations met, $fail failed, $drive driver failures, $mon monitor warnings" + $(if ($faults) { ', emulator faults' } else { '' }) + $(if ($ended) { '' } else { ', did not finish' }) + ")" + $(if ($warps) { "; $warps navigation warps" } else { '' }) + $(if ($misses) { "; $misses crosshair misses (activated directly)" } else { '' })
