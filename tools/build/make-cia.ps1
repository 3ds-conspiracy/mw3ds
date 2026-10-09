# Builds build\mw3ds.cia with the converted game data packed into its RomFS.
# New 3DS settings: 124MB app memory mode, 804MHz CPU, L2 cache.
#   -NoData   the program only (~2 MB): the game reads its data from sdmc:/3ds/mw3ds/data
#             (the whole island copied there by card reader); no Wi-Fi update checks either
#   -Data out\world  pack that data (default out\data, the Balmora area)
param([switch]$NoData, [string]$Data = 'out\data')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'env.ps1')
Set-Location $root
$makeOut = & (Join-Path $PSScriptRoot 'make.ps1')
$makeCode = $LASTEXITCODE
$makeOut | Select-String -Pattern 'error|warning' | ForEach-Object Line
if ($makeCode -ne 0) {
    "make failed (exit code $makeCode). Last lines of its output:"
    $makeOut | Select-Object -Last 30
    exit 1
}

if ($NoData) {
    Remove-Item (Join-Path $root 'build\romfs\data') -Recurse -Force -ErrorAction SilentlyContinue
} else {
    robocopy (Join-Path $root $Data) (Join-Path $root 'build\romfs\data') /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
}
$defs = @{
    APP_TITLE = 'MW3DS'; APP_PRODUCT_CODE = 'CTR-P-MW3D'; APP_UNIQUE_ID = '0xF3D50'
    APP_ROMFS = 'build/romfs'; APP_CATEGORY = 'Application'; APP_USE_ON_SD = 'true'; APP_ENCRYPTED = 'false'
    APP_MEMORY_TYPE = 'Application'; APP_SYSTEM_MODE = '64MB'; APP_SYSTEM_MODE_EXT = '124MB'
    APP_CPU_SPEED = '804MHz'; APP_ENABLE_L2_CACHE = 'true'; APP_VERSION_MAJOR = '0'
}
$args = @('-f', 'cia', '-o', 'build\mw3ds.cia', '-elf', 'mw3ds.elf', '-rsf', 'tools\build\mw3ds.rsf',
          '-icon', 'mw3ds.smdh', '-banner', 'tools\build\banner.bnr', '-exefslogo', '-target', 't')
foreach ($k in $defs.Keys) { $args += "-D$k=$($defs[$k])" }
& (Join-Path (Get-MwEnv 'MW3DS_CTR_TOOLS_DIR' 'the folder with makerom.exe') 'makerom.exe') @args
$makeromCode = $LASTEXITCODE
# The staged copy of the data is only makerom's input: packed into the CIA, it would be a third 1 GB copy on disk
Remove-Item (Join-Path $root 'build\romfs\data') -Recurse -Force -ErrorAction SilentlyContinue
if ($makeromCode -ne 0) { "makerom failed"; exit 1 }
$cia = Get-Item 'build\mw3ds.cia'
"built $($cia.FullName) ($([math]::Round($cia.Length / 1MB, 1)) MB)"
