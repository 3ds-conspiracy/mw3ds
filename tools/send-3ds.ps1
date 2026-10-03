# Sends mw3ds.3dsx to a real 3DS over WiFi. On the 3DS: open Homebrew Launcher, press Y.
# Usage: tools\send-3ds.ps1 [-Ip 192.168.x.x]   (IP is shown on the 3DS netloader screen)
param([string]$Ip)
. (Join-Path $PSScriptRoot 'env.ps1')
$env:MSYSTEM = 'MSYS'
$proj = ConvertTo-MsysPath $mwRoot
$addr = if ($Ip) { "-a $Ip" } else { '' }
& (Join-Path (Get-MwEnv 'MW3DS_MSYS2_DIR' 'your MSYS2 folder') 'usr\bin\bash.exe') -lc "cd $proj && `$DEVKITPRO/tools/bin/3dslink $addr mw3ds.3dsx 2>&1"
exit $LASTEXITCODE
