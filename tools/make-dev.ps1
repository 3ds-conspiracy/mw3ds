# Builds the self-updating development CIA: code only (the game data comes from the PC into
# sdmc:/3ds/mw3ds/data, see source/devupdate.cpp), served by tools/serve-cia.js:
#   build\dev\mw3ds-dev.cia   the CIA (about 1 MB)
#   build\dev\version.txt     its build id; a dev build that differs installs this CIA and restarts
# First install: FBI > Remote Install > Scan QR Code with build\cia-qr-dev.png (once). After that,
# launching the game on the 3DS updates it.
#   tools\make-dev.ps1        after code changes
#   (after tools\level.py nothing is needed: the game fetches changed data files at launch)
param([int]$Port = 8080)
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'env.ps1')
Set-Location $root
& (Join-Path $PSScriptRoot 'make.ps1') | Select-String -Pattern 'error' | ForEach-Object Line
if ($LASTEXITCODE -ne 0) { "build failed"; exit 1 }

$ip = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
       Select-Object -First 1).IPv4Address.IPAddress
if (-not $ip) { "no LAN IPv4 address found"; exit 1 }
$dev = Join-Path $root 'build\dev'
$romfs = Join-Path $dev 'romfs'
New-Item -ItemType Directory -Force $romfs | Out-Null
Get-ChildItem $romfs | Remove-Item -Recurse -Force
$id = "$(git rev-parse --short HEAD)-$(Get-Date -Format 'MMdd-HHmmss')"
Set-Content (Join-Path $romfs 'buildid.txt') $id -Encoding ascii
Set-Content (Join-Path $romfs 'devhost.txt') "$ip $($Port + 2)" -Encoding ascii
Set-Content (Join-Path $romfs 'loghost.txt') "$ip $($Port + 1)" -Encoding ascii

$defs = @{
    APP_TITLE = 'MW3DS'; APP_PRODUCT_CODE = 'CTR-P-MW3D'; APP_UNIQUE_ID = '0xF3D50'
    APP_ROMFS = 'build/dev/romfs'; APP_CATEGORY = 'Application'; APP_USE_ON_SD = 'true'; APP_ENCRYPTED = 'false'
    APP_MEMORY_TYPE = 'Application'; APP_SYSTEM_MODE = '64MB'; APP_SYSTEM_MODE_EXT = '124MB'
    APP_CPU_SPEED = '804MHz'; APP_ENABLE_L2_CACHE = 'true'; APP_VERSION_MAJOR = '0'
}
$cia = Join-Path $dev 'mw3ds-dev.cia'
$tmp = "$cia.tmp"
$args = @('-f', 'cia', '-o', $tmp, '-elf', 'mw3ds.elf', '-rsf', 'tools\mw3ds.rsf',
          '-icon', 'mw3ds.smdh', '-banner', 'tools\banner.bnr', '-exefslogo', '-target', 't')
foreach ($k in $defs.Keys) { $args += "-D$k=$($defs[$k])" }
& (Join-Path (Get-MwEnv 'MW3DS_CTR_TOOLS_DIR' 'the folder with makerom.exe') 'makerom.exe') @args
if ($LASTEXITCODE -ne 0) { "makerom failed"; exit 1 }
Move-Item -Force $tmp $cia
# The version last: a 3DS that sees it can fetch the CIA
Set-Content (Join-Path $dev 'version.txt') $id -Encoding ascii

$qr = Join-Path $root 'build\cia-qr-dev.png'
$url = "https://${ip}:$Port/dev/mw3ds-dev.cia"
python -c "import qrcode`ntry: qrcode.make('$url', box_size=12, border=4).save(r'$qr')`nexcept OSError: pass"
"dev build $id ($([math]::Round((Get-Item $cia).Length / 1KB)) KB); first install: $qr ($url)"
