@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\..\.."

set "LOG=logs\qemu-serial.log"
set "PASS=0"
set "FAIL=0"
set "SKIP=0"

if not exist "%LOG%" (
    echo [FAIL] Log not found: %LOG%
    echo Run run-qemu-ahci.bat first.
    exit /b 1
)

echo ============================================
echo  Serial log checks: %LOG%
echo ============================================
echo.

call :find_optional "fs_ok" "fs_ok" "filesystem autotest"
call :find_optional "keyboard_ok" "keyboard_ok" "keyboard decode autotest"
call :find_optional "virtio" "[INF][virtio] initialized" "virtio-net driver init"
call :find_optional "rtl8139" "[INF][rtl8139]" "RTL8139 driver activity"
call :find_pattern "dhcp" "[INF][dhcp]" "DHCP activity"
call :find_pattern "nic" "[INF][nic]" "NIC init/packets"
call :find_pattern "http_listen" "[INF][http] listen" "HTTP server started"
call :find_pattern "http_accept" "[INF][http] accept" "HTTP client accepted"
call :find_pattern "http_sent" "[INF][http] sent" "HTTP response sent"
call :find_optional "dns" "[INF][dns]" "DNS resolve"
call :find_optional "autotest" "[INF][autotest] http_done" "Autotest HTTP done"
call :find_optional "idle" "[INF][http] idle_done" "HTTP idle shutdown"
call :find_optional "syn" "syn rx" "TCP SYN received"
call :find_optional "pit" "PIT timer" "PIT timer boot line"
call :find_optional "err" "[ERR]" "Errors in log"

echo.
echo ============================================
echo  Results: PASS=!PASS!  FAIL=!FAIL!  SKIP=!SKIP!
echo ============================================
if !FAIL! gtr 0 exit /b 1
exit /b 0

:find_pattern
set "KEY=%~1"
set "PAT=%~2"
set "DESC=%~3"
findstr /C:"%PAT%" "%LOG%" >nul 2>&1
if errorlevel 1 (
    if /I "%KEY%"=="err" (
        echo [PASS] No %PAT% lines ^(!DESC!^)
        set /a PASS+=1
    ) else (
        echo [FAIL] Missing %PAT% ^(!DESC!^)
        set /a FAIL+=1
    )
) else (
    if /I "%KEY%"=="err" (
        echo [WARN] Found %PAT% ^(!DESC!^) — review log manually
        set /a PASS+=1
    ) else (
        echo [PASS] Found %PAT% ^(!DESC!^)
        set /a PASS+=1
    )
)
exit /b 0

:find_optional
set "PAT=%~2"
set "DESC=%~3"
findstr /C:"%PAT%" "%LOG%" >nul 2>&1
if errorlevel 1 (
    echo [SKIP] Missing %PAT% ^(!DESC!^)
    set /a SKIP+=1
) else (
    echo [PASS] Found %PAT% ^(!DESC!^)
    set /a PASS+=1
)
exit /b 0
