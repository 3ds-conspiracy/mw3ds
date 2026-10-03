# Runs story tests one after another and prints each verdict (the main quest chapters by default):
#   tools\test\run-suite.ps1 [-Tests mq-ch1,mq-ch2] [-Data out\world] [-Start 'Seyda Neen'] [-Chained]
param([string[]]$Tests = @(), [string]$Data = 'out\world', [string]$Start = 'Seyda Neen', [int]$Wait = 2400, [switch]$Chained)
. (Join-Path $PSScriptRoot '..\build\env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Tests) { $Tests = Get-ChildItem (Join-Path $root 'tools\test\cases\mq-ch*.txt') | Sort-Object { [int]($_.BaseName -replace '\D', '') } | ForEach-Object { $_.BaseName } }
# -Chained: each chapter starts from the save the one before it ended with (its own setup then only raising
# what's behind: CHAIN), so the hand-offs between chapters get played rather than warped
if ($Chained) {
    $prev = $null
    $chainTests = @()
    foreach ($t in $Tests) {
        $lines = Get-Content (Join-Path $root "tools\test\cases\$t.txt")
        $out = @()
        if ($prev) {
            $out += '3 0 0 0 0 MSG', '0.5 0 0 0 0 LOAD:chain', '4 0 0 0', '0.3 0 0 0 0 CHAIN'
            $lines = $lines | Select-Object -Skip 1
        }
        $out += $lines
        $out += '1 0 0 0 0 SAVE:chain', '1 0 0 0'
        $name = "chain-$t"
        Set-Content (Join-Path $root "tools\test\cases\$name.txt") $out -Encoding ascii
        $chainTests += $name
        $prev = $t
    }
    $Tests = $chainTests
}
$logs = Join-Path $root 'build\suite-logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$log = Join-Path (Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder') 'user\sdmc\3ds\mw3ds\log.txt'
foreach ($t in $Tests) {
    $out = & (Join-Path $PSScriptRoot 'run-test.ps1') $t -Data $Data -Start $Start -Wait $Wait
    Copy-Item $log (Join-Path $logs "$t.log") -Force
    $verdict = $out | Where-Object { $_ -like 'RESULT:*' }
    "{0,-10} {1}" -f $t, $verdict
    $out | Where-Object { $_ -match 'expect: FAIL|drive: FAIL|monitor:|emulator faults' } | ForEach-Object { "           $_" }
}
