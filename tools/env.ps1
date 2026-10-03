# Dot-source from the other scripts:  . (Join-Path $PSScriptRoot 'env.ps1')
# Loads the repo's .env (see .env.example) into the process environment (variables already set win) and
# provides Get-MwEnv for the settings the scripts need.
$envFile = Join-Path (Split-Path $PSScriptRoot -Parent) '.env'
if (Test-Path $envFile) {
    foreach ($line in Get-Content $envFile) {
        $line = $line.Trim()
        if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
        $k, $v = $line.Split('=', 2)
        $v = $v.Trim().Trim('"').Trim("'")
        if ($v -and -not [Environment]::GetEnvironmentVariable($k.Trim())) { [Environment]::SetEnvironmentVariable($k.Trim(), $v) }
    }
}
function Get-MwEnv([string]$Name, [string]$What) {
    $v = [Environment]::GetEnvironmentVariable($Name)
    if (-not $v) { throw "$Name is not set: $What. Put it in .env (copy .env.example) or the environment." }
    $v
}
# C:\a\b -> /c/a/b (the path as MSYS2's bash sees it)
function ConvertTo-MsysPath([string]$Path) {
    $p = (Resolve-Path $Path).Path.Replace('\', '/')
    if ($p -match '^([A-Za-z]):(.*)$') { $p = '/' + $Matches[1].ToLower() + $Matches[2] }
    $p
}
$mwRoot = Split-Path $PSScriptRoot -Parent
