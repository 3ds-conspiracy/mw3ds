# Runs a scripted test from tools\test\cases in Azahar and prints its result lines (check: / journal / crime ...).
#   tools\test\run-test.ps1 fg-rathunt                 (starts in Balmora, the Balmora-area data)
#   tools\test\run-test.ps1 tg-bragor -Data out\world  (the whole island)
#   tools\test\run-test.ps1 mainquest -Start "Seyda Neen"
param([Parameter(Mandatory = $true)][string]$Test, [string]$Start = 'Balmora', [string]$Data = 'out\data', [int]$Wait = 300, [string]$App = '',
      [string]$Emu = '', [switch]$Fast, [switch]$Native, [string]$Sd = '',
      [string]$Pattern = 'check:|not offered|failed|expelled|crime:|expect:|monitor:|drive: (FAIL|stuck|crosshair miss)|was fighting|script error')
. (Join-Path $PSScriptRoot '..\build\env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Native -and -not $Emu) { $Emu = Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder' }
$inputs = Get-Content (Join-Path $root "tools\test\cases\$Test.txt")
if ($Native) {
    # The game built for the PC (tools\test\native\build.py): same case file, same log lines, no emulator
    $sd = if ($Sd) { $Sd } else { Join-Path $root 'build\native\sd' }   # -Sd: its own SD folder, so runs can go in parallel
    $sdApp = Join-Path $sd '3ds\mw3ds'
    # A clean SD folder every run: saves and settings the case before left behind change what the next one starts in
    if (Test-Path $sdApp) { Remove-Item $sdApp -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force $sdApp | Out-Null
    Set-Content (Join-Path $sdApp 'autoinput.txt') $inputs -Encoding ascii
    Set-Content (Join-Path $sdApp 'start.txt') $Start -Encoding ascii
    $env:NATIVE_SD = $sd
    $env:MW3DS_DATA = Join-Path $root $Data
    $p = Start-Process -FilePath (Join-Path $root 'build\native\mw3ds-native.exe') -WorkingDirectory $root -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit($Wait * 1000)) { $p.Kill() }
    $log = Join-Path $sdApp 'log.txt'
} else {
    & (Join-Path $PSScriptRoot 'run-emu.ps1') -Start $Start -Data $Data -Inputs $inputs -Wait $Wait -App $App -Emu $Emu -Fast:$Fast | Out-Null
    $log = Join-Path $Emu 'user\sdmc\3ds\mw3ds\log.txt'
}
"== $Test"
Select-String -Path $log -Pattern $Pattern | ForEach-Object { $_.Line }
if (-not (Select-String -Path $log -Pattern 'autoinput: end' -Quiet)) { "(did not reach the end)" }
# Memory faults the emulator saw (reads / writes of unmapped memory: a crash on hardware), by code address
$faults = $null
if (-not $Native) {
    $emuLog = Join-Path $Emu 'user\log\azahar_log.txt'
    $faults = Select-String -Path $emuLog -Pattern 'Unmapped\w+ @ \S+ at PC (0x[0-9A-Fa-f]+)|Unreachable code' -ErrorAction SilentlyContinue
}
if ($faults) {
    $pcs = $faults | ForEach-Object { if ($_.Matches[0].Groups[1].Success) { $_.Matches[0].Groups[1].Value } else { 'gpu-unreachable' } } | Group-Object | Sort-Object Count -Descending
    "emulator faults: " + (($pcs | Select-Object -First 5 | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ') + "  (tools\build\addr2line.ps1 <pc>)"
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
