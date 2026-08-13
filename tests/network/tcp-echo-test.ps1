# TCP echo test for KnitOS socktest / tcp listen (run server in guest first).
param(
    [string]$GuestHost = "10.0.2.15",
    [int]$Port = 9001,
    [string]$Message = "hello-tcp-echo",
    [int]$TimeoutMs = 5000
)

$ErrorActionPreference = "Stop"

Write-Host "============================================"
Write-Host " KnitOS TCP echo test -> ${GuestHost}:${Port}"
Write-Host " Guest: socktest tcp $Port"
Write-Host "============================================"

$client = New-Object System.Net.Sockets.TcpClient
$connect = $client.BeginConnect($GuestHost, $Port, $null, $null)
if (-not $connect.AsyncWaitHandle.WaitOne($TimeoutMs)) {
    Write-Host "[FAIL] Connect timeout"
    exit 1
}
$client.EndConnect($connect)

$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII
$bytes = $enc.GetBytes($Message)
$stream.Write($bytes, 0, $bytes.Length)

$buf = New-Object byte[] 256
$read = $stream.Read($buf, 0, $buf.Length)
$reply = $enc.GetString($buf, 0, $read)

$stream.Close()
$client.Close()

if ($reply -eq $Message) {
    Write-Host "[PASS] Echo OK: $reply"
    exit 0
}

Write-Host "[FAIL] Expected '$Message', got '$reply'"
exit 1
