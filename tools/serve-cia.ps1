# Serves build\mw3ds.cia over the local network and shows a QR code for FBI's
# "Remote Install > Scan QR Code". Stop the server with: tools\serve-cia.ps1 -Stop
# Serves HTTPS by default so FBI downloads through libcurl, which survives Wi-Fi stalls
# (see serve-cia.js); -Http serves plain HTTP, where FBI gives up after a 15 s stall.
#   -Data out\world    dev builds sync the whole island's data instead of the Balmora area's
param([int]$Port = 8080, [switch]$Stop, [switch]$Http, [string]$Data = 'out\data')

$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$pidFile = Join-Path $build 'serve.pid'

if (Test-Path $pidFile) {
    Stop-Process -Id (Get-Content $pidFile) -Force -ErrorAction SilentlyContinue
    Remove-Item $pidFile
}
if ($Stop) { "server stopped"; exit 0 }

# LAN address of the interface that has the default route
$ip = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
       Select-Object -First 1).IPv4Address.IPAddress
if (-not $ip) { "no LAN IPv4 address found"; exit 1 }
$nodeArgs = @((Join-Path $PSScriptRoot 'serve-cia.js'), $ip, $Port, $build)
if ($Http) {
    $url = "http://${ip}:$Port/mw3ds.cia"
} else {
    # self-signed certificate, made once (FBI doesn't verify it)
    $cert = Join-Path $build 'serve-cert.pem'
    $key = Join-Path $build 'serve-key.pem'
    if (-not ((Test-Path $cert) -and (Test-Path $key))) {
        $openssl = (Get-Command openssl -ErrorAction SilentlyContinue).Source
        if (-not $openssl) { $openssl = Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe' }
        if (-not (Test-Path $openssl)) { "openssl not found (install Git for Windows, or use -Http)"; exit 1 }
        & $openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj '/CN=mw3ds' -keyout $key -out $cert 2>$null
        if (-not (Test-Path $cert)) { "openssl failed to make a certificate"; exit 1 }
    }
    $nodeArgs += $cert, $key
    $url = "https://${ip}:$Port/mw3ds.cia"
}

# serve-cia.js (Node): 4 MB read buffers, 6-hour idle timeout, keepalive, Range requests, progress in build\serve.log
$env:MW3DS_DATA = $Data
$server = Start-Process node -ArgumentList $nodeArgs -WindowStyle Hidden -PassThru
Set-Content $pidFile $server.Id

$qr = Join-Path $build ('cia-qr-' + ($url -split ':')[0] + '.png')
# (the old picture stays when a viewer has it open; the URL rarely changes)
python -c "import qrcode`ntry: qrcode.make('$url', box_size=12, border=4).save(r'$qr')`nexcept OSError: pass"
Start-Process $qr
"serving $url (pid $($server.Id)); QR code: $qr; log: build\serve.log"
