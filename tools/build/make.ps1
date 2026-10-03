# Runs make inside the MSYS2 devkitPro environment. Usage: tools\build\make.ps1 [target]
. (Join-Path $PSScriptRoot 'env.ps1')
$env:MSYSTEM = 'MSYS'
$proj = ConvertTo-MsysPath $mwRoot
& (Join-Path (Get-MwEnv 'MW3DS_MSYS2_DIR' 'your MSYS2 folder') 'usr\bin\bash.exe') -lc "cd $proj && make -j4 $($args -join ' ') 2>&1"
exit $LASTEXITCODE
