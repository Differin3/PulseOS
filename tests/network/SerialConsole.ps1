# Bidirectional COM1 session to MyOS (QEMU -serial tcp:127.0.0.1:4444,server,nowait)
param(
    [string]$SerialHost = "127.0.0.1",
    [int]$Port = 4444,
    [string]$LogFile = ""
)

$ErrorActionPreference = "Stop"

function Connect-Serial {
    param([string]$Address, [int]$TcpPort, [int]$TimeoutSec = 60)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $iar = $client.BeginConnect($Address, $TcpPort, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(2000, $false) -and $client.Connected) {
                $client.EndConnect($iar)
                $client.NoDelay = $true
                return $client
            }
            $client.Close()
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    throw "Serial connect timeout ${Address}:${TcpPort}"
}

function Start-SerialReader {
    param($Stream, [System.Text.StringBuilder]$Buffer, $LogPath)
    $script:readerRunning = $true
    $job = [System.Threading.Thread]::new({
        param($s, $buf, $log, $runningFlag)
        $tmp = New-Object byte[] 4096
        while ($runningFlag.Value) {
            try {
                if (-not $s.DataAvailable) {
                    Start-Sleep -Milliseconds 20
                    continue
                }
                $n = $s.Read($tmp, 0, $tmp.Length)
                if ($n -le 0) { break }
                $text = [System.Text.Encoding]::UTF8.GetString($tmp, 0, $n)
                [void]$buf.Append($text)
                if ($log) {
                    Add-Content -Path $log -Value $text -NoNewline -Encoding UTF8
                }
            } catch { break }
        }
    })
    $flag = [ref]([bool]$true)
    $job.IsBackground = $true
    $job.Start($Stream, $Buffer, $LogPath, $flag)
    return @{ Thread = $job; Flag = $flag }
}

function Send-SerialLine {
    param($Stream, [string]$Line)
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Line + "`n")
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

function Wait-SerialPattern {
    param(
        [System.Text.StringBuilder]$Buffer,
        [string[]]$Patterns,
        [int]$TimeoutSec = 120
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $text = $Buffer.ToString()
        foreach ($p in $Patterns) {
            if ($text -match [regex]::Escape($p)) {
                return $true
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

if ($MyInvocation.InvocationName -ne '.') {
    Write-Host "MyOS serial console -> ${SerialHost}:${Port}"
    Write-Host "Type commands (exit with Ctrl+C). Log: $LogFile"
    $client = Connect-Serial -Address $SerialHost -TcpPort $Port
    $stream = $client.GetStream()
    $buf = New-Object System.Text.StringBuilder
    if ($LogFile) { Set-Content -Path $LogFile -Value "" -Encoding UTF8 }
    $reader = Start-SerialReader -Stream $stream -Buffer $buf -LogPath $LogFile
    try {
        while ($true) {
            if ($buf.Length -gt 0) {
                $chunk = $buf.ToString()
                [void]$buf.Clear()
                Write-Host $chunk -NoNewline
            }
            if ([Console]::KeyAvailable) {
                $line = Read-Host
                if ($line) { Send-SerialLine -Stream $stream -Line $line }
            }
            Start-Sleep -Milliseconds 50
        }
    } finally {
        $reader.Flag.Value = $false
        $client.Close()
    }
}
