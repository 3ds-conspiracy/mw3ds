# Runs mw3ds.3dsx in Azahar and captures what it draws, via the app's own screenshots.
#   tools\test\run-emu.ps1                 one shot (both screens) after 3 s -> build\emu-shot.png
#   tools\test\run-emu.ps1 -Cams "spawn","x y z yaw pitch",...
#                                     one top-screen shot per pose, tiled 2-wide -> build\emu-cams.png
#                                     ("@Cell name" switches cell for the poses after it)
# Converted game data in out\data (or -Data <folder>) is mirrored onto the emulated SD card first.
#   -Start "Seyda Neen"               start in that cell, skipping character creation
#   -Emu <folder>                     another Azahar copy (its own SD card and log): a spot check beside a
#                                     sweep; only that copy's emulator is stopped
#   -Fast                             no speed cap, no vsync (the ini is put back afterwards)
param([string[]]$Cams, [string[]]$Inputs, [int]$Wait = 60, [switch]$Keep, [string]$Start, [string]$Data = 'out\data', [string]$App = '',
      [string]$Emu = '', [switch]$Fast)

. (Join-Path $PSScriptRoot '..\build\env.ps1')
$root  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$defaultEmu = Get-MwEnv 'MW3DS_EMU_DIR' 'the Azahar folder'
if (-not $Emu) { $Emu = $defaultEmu }
$azDir = $Emu
$app   = if ($App) { $App } else { Join-Path $root 'mw3ds.3dsx' }
$sd    = Join-Path $azDir 'user\sdmc\3ds\mw3ds'
$log   = Join-Path $sd 'log.txt'
$data  = Join-Path $root $Data         # -Data out\world: the whole-island trial conversion
New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null

Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($azDir, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force
New-Item -ItemType Directory -Force $sd | Out-Null
Remove-Item $log, (Join-Path $sd 'shot*.bmp'), (Join-Path $sd 'autocam.txt'), (Join-Path $sd 'autoshot') -Force -ErrorAction SilentlyContinue
# The game data on the emulated SD card: the main copy mirrors it; the other copies (-Emu) link to the
# folder itself (a directory junction: no second 1 GB copy; the game only reads it)
$sdData = Join-Path $sd 'data'
if (Test-Path $data) {
    if ($azDir -ieq $defaultEmu) {
        robocopy $data $sdData /MIR /NJH /NJS /NFL /NDL /NP | Out-Null
    } else {
        $item = Get-Item $sdData -Force -ErrorAction SilentlyContinue
        $target = if ($item -and $item.LinkType -eq 'Junction') { @($item.Target)[0] } else { $null }
        if (-not $target -or ((Resolve-Path $data).Path -ine $target.TrimEnd('\'))) {
            # rmdir removes only the link (or an old real copy with /s): never the linked folder's files
            if ($item -and $item.LinkType -eq 'Junction') { cmd /c rmdir "$sdData" | Out-Null }
            elseif ($item) { cmd /c rmdir /s /q "$sdData" | Out-Null }
            New-Item -ItemType Junction -Path $sdData -Target (Resolve-Path $data).Path | Out-Null
        }
    }
}

# -Fast: the emulator's speed cap and vsync off for this run (the ini is restored after the run)
$ini = Join-Path $azDir 'user\config\qt-config.ini'
$iniBackup = $null
if ($Fast -and (Test-Path $ini)) {
    $iniBackup = Get-Content $ini -Raw
    $text = $iniBackup
    foreach ($kv in @(@('use_vsync', 'false'), @('frame_limit', '900'))) {
        $text = $text.Replace("$($kv[0])\default=true", "$($kv[0])\default=false")
        $text = [regex]::Replace($text, "(?m)^$($kv[0])=[^\r\n]*", "$($kv[0])=$($kv[1])")
    }
    [IO.File]::WriteAllText($ini, $text)
}

try {
Remove-Item (Join-Path $sd 'autoinput.txt') -Force -ErrorAction SilentlyContinue
$startFile = Join-Path $sd 'start.txt'
if ($Start) { Set-Content -Path $startFile -Value $Start -Encoding ascii } else { Remove-Item $startFile -Force -ErrorAction SilentlyContinue }
if ($Cams) {
    Set-Content -Path (Join-Path $sd 'autocam.txt') -Value $Cams -Encoding ascii
    $donePattern = 'autocam done'
} elseif ($Inputs) {
    # Scripted stick input "seconds moveX moveY lookX [jump]"; ends with a shot.bmp
    Set-Content -Path (Join-Path $sd 'autoinput.txt') -Value $Inputs -Encoding ascii
    $donePattern = 'autoshot'
} else {
    New-Item -ItemType File -Force (Join-Path $sd 'autoshot') | Out-Null
    $donePattern = 'autoshot'
}

$proc = Start-Process -FilePath (Join-Path $azDir 'azahar.exe') -ArgumentList "`"$app`"" -PassThru -WindowStyle Minimized

$deadline = (Get-Date).AddSeconds($Wait)
$done = $false
$stalled = $false
while ((Get-Date) -lt $deadline -and -not $done) {
    $done = (Test-Path $log) -and (Select-String -Path $log -Pattern $donePattern -Quiet)
    if (-not $done) {
        Start-Sleep -Milliseconds 500
        # Give up early when the emulator is gone or the game stopped writing its log (a hang)
        if ($proc.HasExited) { $stalled = $true; break }
        if ((Test-Path $log) -and ((Get-Date) - (Get-Item $log).LastWriteTime).TotalSeconds -gt 180) { $stalled = $true; break }
    }
}
if ($stalled) { "stalled: emulator exited or log quiet for 3 min" }

Add-Type -AssemblyName System.Drawing
if ($done -and ($Cams -or @(Get-ChildItem $sd -Filter 'shot_*.bmp').Count -gt 0)) {
    $shots = @(Get-ChildItem $sd -Filter 'shot_*.bmp' | Sort-Object Name)
    $cols = [Math]::Min(2, $shots.Count); $rows = [Math]::Ceiling($shots.Count / 2)
    $grid = New-Object System.Drawing.Bitmap (400 * $cols), (240 * $rows)
    $g = [System.Drawing.Graphics]::FromImage($grid)
    for ($i = 0; $i -lt $shots.Count; $i++) {
        $img = [System.Drawing.Image]::FromFile($shots[$i].FullName)
        $dst = New-Object System.Drawing.Rectangle (400 * ($i % 2)), (240 * [Math]::Floor($i / 2)), 400, 240
        $g.DrawImage($img, $dst, (New-Object System.Drawing.Rectangle 0, 0, 400, 240), [System.Drawing.GraphicsUnit]::Pixel)
        $img.Dispose()
    }
    $g.Dispose()
    $out = Join-Path $root 'build\emu-cams.png'
    $grid.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $grid.Dispose()
    "screenshot grid ($($shots.Count)): $out"
} elseif ($done -and (Test-Path (Join-Path $sd 'shot.bmp'))) {
    $out = Join-Path $root 'build\emu-shot.png'
    $img = [System.Drawing.Image]::FromFile((Join-Path $sd 'shot.bmp'))
    $img.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
    "screenshot: $out"
} else { "no screenshot after $Wait s" }

if (Test-Path $log) { Get-Content $log | Where-Object { $_ -notmatch '^\[\s*\d+\] fps' } | Select-Object -Last 40; Get-Content $log | Where-Object { $_ -match '\] fps' } | Select-Object -Last 1 } else { "no app log" }

Remove-Item (Join-Path $sd 'autoshot'), (Join-Path $sd 'autocam.txt'), (Join-Path $sd 'autoinput.txt') -Force -ErrorAction SilentlyContinue
if (-not $Keep) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
} finally {
    # -Fast: the ini goes back even if the run failed
    if ($null -ne $iniBackup) { Start-Sleep -Milliseconds 500; [IO.File]::WriteAllText($ini, $iniBackup) }
}
