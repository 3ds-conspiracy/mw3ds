# Builds output\mw3ds.cia in Docker from the Morrowind install named in .env (MW3DS_MORROWIND_DIR).
#   tools\docker\build.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\env.ps1')
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$morrowind = Get-MwEnv 'MW3DS_MORROWIND_DIR' 'the folder of your Morrowind install'
if (-not (Test-Path (Join-Path $morrowind 'Data Files\Morrowind.esm'))) { throw "Morrowind.esm not found under $morrowind\Data Files" }
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'docker not found on PATH: install Docker Desktop, start it, and open a new terminal (a terminal opened before the install does not see it)' }
docker info *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker is installed but not running: start Docker Desktop and wait for it to say it is running' }
$env:DOCKER_BUILDKIT = '1'
docker build -f tools/docker/Dockerfile --target cia --build-context "morrowind=$morrowind" --output type=local,dest=output .
if ($LASTEXITCODE -ne 0) { throw "docker build failed (exit code $LASTEXITCODE). Send the whole output: the last 'decoding' line names the sound file it stopped on." }
"Built output\mw3ds.cia ($([math]::Round((Get-Item output\mw3ds.cia).Length / 1MB, 1)) MB)"
