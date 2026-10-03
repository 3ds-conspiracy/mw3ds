# Checks the build requirements and installs what it can:
#   tools\setup.ps1          install the Python packages and the devkitPro packages, then report what is left
#   tools\setup.ps1 -Check   only report, install nothing
# Reads .env (created from .env.example if it is missing).
param([switch]$Check)
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$problems = 0
function Ok($m)   { "  ok       $m" }
function Fix($m)  { "  MISSING  $m"; $script:problems++ }

if (-not (Test-Path '.env')) {
    Copy-Item '.env.example' '.env'
    "Created .env from .env.example: fill in the paths, then run this again."
}
. (Join-Path $PSScriptRoot 'env.ps1')

"== Python"
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { Fix 'python 3.10+ (https://www.python.org/downloads/)' }
else {
    Ok (python --version)
    if ($Check) {
        python -c "import numpy, PIL, miniaudio" 2>$null
        if ($LASTEXITCODE -eq 0) { Ok 'numpy, pillow, miniaudio' } else { Fix 'Python packages: run tools\setup.ps1 (pip install -r requirements.txt)' }
    } else {
        python -m pip install -r requirements.txt
        if ($LASTEXITCODE -ne 0) { Fix 'pip install -r requirements.txt failed' } else { Ok 'Python packages installed' }
    }
}

"== Morrowind"
$mw = [Environment]::GetEnvironmentVariable('MW3DS_MORROWIND_DIR')
if (-not $mw) { Fix 'MW3DS_MORROWIND_DIR is not set in .env' }
elseif (Test-Path (Join-Path $mw 'Data Files\Morrowind.esm')) { Ok "Morrowind.esm in $mw" }
else { Fix "Morrowind.esm not found under $mw\Data Files" }

"== MSYS2 + devkitPro"
$msys = [Environment]::GetEnvironmentVariable('MW3DS_MSYS2_DIR')
$bash = if ($msys) { Join-Path $msys 'usr\bin\bash.exe' }
if (-not $bash -or -not (Test-Path $bash)) {
    Fix 'MSYS2 (https://www.msys2.org/): install it and set MW3DS_MSYS2_DIR in .env'
} else {
    $env:MSYSTEM = 'MSYS'
    & $bash -lc "grep -q '^\[dkp-libs\]' /etc/pacman.conf"
    if ($LASTEXITCODE -ne 0) {
        Fix 'devkitPro is not set up in this MSYS2: follow https://devkitpro.org/wiki/devkitPro_pacman (add the repositories and keyring)'
    } elseif ($Check) {
        & $bash -lc "pacman -Qg 3ds-dev >/dev/null 2>&1"
        if ($LASTEXITCODE -eq 0) { Ok '3ds-dev packages' } else { Fix '3ds-dev packages: run tools\setup.ps1' }
    } else {
        & $bash -lc "pacman -S --needed --noconfirm 3ds-dev"
        if ($LASTEXITCODE -ne 0) { Fix 'pacman -S 3ds-dev failed' } else { Ok '3ds-dev packages installed' }
    }
}

"== makerom (for the CIA)"
$ctr = [Environment]::GetEnvironmentVariable('MW3DS_CTR_TOOLS_DIR')
if ($ctr -and (Test-Path (Join-Path $ctr 'makerom.exe'))) { Ok "makerom.exe in $ctr" }
elseif ($Check) { Fix 'makerom.exe: run tools\setup.ps1 to download it' }
else {
    # Download the latest makerom release from Project_CTR into MW3DS_CTR_TOOLS_DIR (default: .tools\ctr in the repo)
    $dest = if ($ctr) { $ctr } else { Join-Path $root '.tools\ctr' }
    try {
        $rel = Invoke-RestMethod 'https://api.github.com/repos/3DSGuy/Project_CTR/releases' -Headers @{ 'User-Agent' = 'mw3ds-setup' } |
            ForEach-Object { $_ } | Where-Object { $_.tag_name -like 'makerom-*' } | Select-Object -First 1
        $asset = $rel.assets | Where-Object { $_.name -like '*win_x86_64.zip' } | Select-Object -First 1
        $zip = Join-Path ([IO.Path]::GetTempPath()) $asset.name
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip -UseBasicParsing
        New-Item -ItemType Directory -Force $dest | Out-Null
        Expand-Archive $zip -DestinationPath $dest -Force
        Remove-Item $zip -Force
        if (-not (Test-Path (Join-Path $dest 'makerom.exe'))) { throw 'makerom.exe is not in the download' }
        if (-not $ctr) {
            Add-Content '.env' "MW3DS_CTR_TOOLS_DIR=$dest"
            [Environment]::SetEnvironmentVariable('MW3DS_CTR_TOOLS_DIR', $dest)
        }
        Ok "downloaded $($rel.tag_name) to $dest"
    } catch {
        Fix "makerom.exe could not be downloaded ($($_.Exception.Message)): get it from https://github.com/3DSGuy/Project_CTR/releases"
    }
}

"== Azahar (only for running tests / the emulator)"
$emu = [Environment]::GetEnvironmentVariable('MW3DS_EMU_DIR')
if ($emu -and (Test-Path (Join-Path $emu 'azahar.exe'))) { Ok "azahar.exe in $emu" }
else { "  optional azahar.exe: https://azahar-emu.org/ , portable install, then set MW3DS_EMU_DIR in .env" }

""
if ($problems) { "$problems thing(s) to fix above." ; exit 1 } else { 'Everything needed to build is in place.' }
