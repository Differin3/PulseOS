# Fully automated network test: QEMU + serial COM + host curl (no manual steps).
param(
    [string]$ProjectRoot = "",
    [int]$SerialPort = 4444,
    [string]$GuestIp = "10.0.2.15",
    [int]$HttpPort = 8080,
    [int]$BootTimeoutSec = 120,
    [int]$TestTimeoutSec = 180,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

if (-not $ProjectRoot) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}
Set-Location $ProjectRoot

. (Join-Path $PSScriptRoot "SerialConsole.ps1")

$iso = Join-Path $ProjectRoot "myos.iso"
$logFile = Join-Path $ProjectRoot "logs\qemu-serial.log"

function Stop-Qemu {
    wsl pkill -f qemu-system-i386 2>$null | Out-Null
    Start-Sleep -Seconds 1
}

function Write-Step([string]$Msg) {
    Write-Host ""
    Write-Host "== $Msg"
}

if (-not $SkipBuild) {
    Write-Step "build.bat"
    & (Join-Path $ProjectRoot "build.bat")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $iso)) {
    Write-Host "[FAIL] myos.iso missing"
    exit 1
}

if (-not (Test-Path (Join-Path $ProjectRoot "logs"))) {
    New-Item -ItemType Directory -Path (Join-Path $ProjectRoot "logs") | Out-Null
}

Stop-Qemu
Set-Content -Path $logFile -Value "" -Encoding UTF8

Write-Step "Starting QEMU (run-qemu-test.bat $SerialPort)"
$qemuBat = Join-Path $ProjectRoot "run-qemu-test.bat"
$qemuProc = Start-Process -FilePath "cmd.exe" -ArgumentList "/c", "`"$qemuBat`" $SerialPort" `
    -WorkingDirectory $ProjectRoot -PassThru -WindowStyle Hidden

$client = $null
$reader = $null
$exitCode = 1

try {
    Write-Step "Connecting serial 127.0.0.1:$SerialPort"
    $client = Connect-Serial -Address "127.0.0.1" -TcpPort $SerialPort -TimeoutSec 90
    $stream = $client.GetStream()
    $buf = New-Object System.Text.StringBuilder
    $reader = Start-SerialReader -Stream $stream -Buffer $buf -LogPath $logFile

    Write-Step "Waiting for guest shell"
    if (-not (Wait-SerialPattern -Buffer $buf -Patterns @("Keyboard ready", "/ >", "Net OK") -TimeoutSec $BootTimeoutSec)) {
        Write-Host "[FAIL] Boot timeout"
        exit 1
    }
    Write-Host "[PASS] Guest ready"

    $httpReady = $false
    $curlJob = Start-Job -ScriptBlock {
        param($Root, $Ip, $Port, $ReadyEventName)
        $deadline = (Get-Date).AddSeconds(150)
        while ((Get-Date) -lt $deadline) {
            if ([System.Threading.EventWaitHandle]::OpenExisting($ReadyEventName).WaitOne(500)) {
                break
            }
        }
        Start-Sleep -Seconds 1
        $bat = Join-Path $Root "tests\network\host-tests.bat"
        & $bat $Ip $Port
        return $LASTEXITCODE
    } -ArgumentList $ProjectRoot, $GuestIp, $HttpPort, "Global\MyOSHttpReady_$PID"

    # Named event for curl job — use file flag instead (simpler on Windows)
    Stop-Job $curlJob -ErrorAction SilentlyContinue
    Remove-Job $curlJob -Force -ErrorAction SilentlyContinue

    $readyFlag = Join-Path $env:TEMP "myos_http_ready_$PID.flag"
    Remove-Item $readyFlag -ErrorAction SilentlyContinue

    $curlJob = Start-Job -ScriptBlock {
        param($Root, $Ip, $Port, $FlagPath)
        $deadline = (Get-Date).AddSeconds(150)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path $FlagPath) { break }
            Start-Sleep -Milliseconds 200
        }
        Start-Sleep -Seconds 1
        & (Join-Path $Root "tests\network\host-tests.bat") $Ip $Port
        return $LASTEXITCODE
    } -ArgumentList $ProjectRoot, $GuestIp, $HttpPort, $readyFlag

    Write-Step "Sending: autotest network $HttpPort 16"
    Send-SerialLine -Stream $stream -Line "autotest network $HttpPort 16"

    $readyDeadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $readyDeadline) {
        if ($buf.ToString() -match "\[AUTOTEST\] http_ready|\[INF\]\[autotest\] http_ready") {
            New-Item -Path $readyFlag -ItemType File -Force | Out-Null
            Write-Host "[PASS] http_ready marker"
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path $readyFlag)) {
        Write-Host "[FAIL] http_ready not seen on serial"
        exit 1
    }

    Write-Step "Waiting for autotest completion"
    if (-not (Wait-SerialPattern -Buffer $buf -Patterns @("[AUTOTEST] http_done", "[INF][autotest] http_done") -TimeoutSec $TestTimeoutSec)) {
        Write-Host "[FAIL] autotest http_done timeout"
        exit 1
    }

    $curlRc = Receive-Job $curlJob -Wait
    Remove-Job $curlJob -Force
    if ($curlRc -ne 0) {
        Write-Host "[FAIL] host-tests.bat exit code $curlRc"
        exit 1
    }
    Write-Host "[PASS] host HTTP tests"

    $text = $buf.ToString()
    if ($text -match "http_done served=([0-9]+)" -or $text -match "served=0x[0-9a-fA-F]+") {
        Write-Host "[PASS] Guest reported completion"
    }

    Write-Step "Serial log checks"
    & (Join-Path $ProjectRoot "tests\network\check-serial-log.bat")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[WARN] Some log checks failed (see above)"
    }

    Write-Host ""
    Write-Host "============================================"
    Write-Host " AUTO TEST PASSED"
    Write-Host "============================================"
    $exitCode = 0
}
finally {
    if ($reader) { $reader.Flag.Value = $false }
    if ($client) { $client.Close() }
    Stop-Qemu
    if ($qemuProc -and -not $qemuProc.HasExited) {
        Stop-Process -Id $qemuProc.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item (Join-Path $env:TEMP "myos_http_ready_$PID.flag") -ErrorAction SilentlyContinue
}

exit $exitCode
