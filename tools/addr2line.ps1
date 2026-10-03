# Code address(es) -> function and line in mw3ds.elf:  tools\addr2line.ps1 0x001DC63C [...]
. (Join-Path $PSScriptRoot 'env.ps1')
$env:MSYSTEM = 'MSYS'
$proj = ConvertTo-MsysPath $mwRoot
& (Join-Path (Get-MwEnv 'MW3DS_MSYS2_DIR' 'your MSYS2 folder') 'usr\bin\bash.exe') -lc "cd $proj && /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -C -i -e mw3ds.elf $($args -join ' ')"
