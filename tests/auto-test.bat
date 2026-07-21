@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.."

if not exist "logs" mkdir "logs"

echo ============================================================
echo   MyOS — automated network test (CMD -^> WSL -^> QEMU)
echo ============================================================
echo.
echo   From CMD in project folder:
echo     build-test.bat              build + full auto test
echo     build-test.bat rebuild      clean + build + test
echo     test.bat --skip-build       test only ^(kernel already built^)
echo.
echo   Manual ^(two windows^):
echo     1. build.bat
echo     2. run-qemu-test.bat
echo     3. In QEMU: autotest network 8080 16
echo        ^(server exits on idle after host tests^)
echo        ^(or httpserver 8080 32^)
echo     4. tests\network\host-tests.bat 10.0.2.15 8080
echo        ^(hostfwd: host-tests.bat 127.0.0.1 8080^)
echo.
echo   Logs:  logs\auto-test.log   logs\qemu-serial.log
echo   View:  view-auto-test-log.bat
echo.
echo   Env ^(optional before run^):
echo     set MYOS_VERBOSE=1      live serial in console ^(default^)
echo     set MYOS_HEADLESS=1     hide QEMU window
echo     set MYOS_DIAG=1         extra ss/curl probes on failure
echo     set CURL_HOST=127.0.0.1 hostfwd target ^(default^)
echo     set HTTP_MAX_REQUESTS=16  guest HTTP request limit
echo.

if not defined MYOS_VERBOSE set "MYOS_VERBOSE=1"
if not defined CURL_HOST set "CURL_HOST=127.0.0.1"
if not defined GUEST_IP set "GUEST_IP=10.0.2.15"
if not defined HTTP_PORT set "HTTP_PORT=8080"
if not defined HTTP_MAX_REQUESTS set "HTTP_MAX_REQUESTS=32"

set "WSL_DIR="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%" 2^>nul`) do set "WSL_DIR=%%P"
if not defined WSL_DIR (
    echo [FAIL] WSL not available. Run setup-wsl.bat or install Ubuntu WSL.
    exit /b 1
)

echo [auto-test] WSL: !WSL_DIR!
echo [auto-test] CURL_HOST=!CURL_HOST! GUEST_IP=!GUEST_IP! HTTP_PORT=!HTTP_PORT! HTTP_MAX=!HTTP_MAX_REQUESTS!
echo.

wsl sed -i "s/\r$//" "!WSL_DIR!/tests/network/auto-network-test.sh" "!WSL_DIR!/tests/network/host-tests.sh" 2>nul

wsl /usr/bin/env MYOS_VERBOSE=!MYOS_VERBOSE! MYOS_DIAG=!MYOS_DIAG! CURL_HOST=!CURL_HOST! GUEST_IP=!GUEST_IP! HTTP_PORT=!HTTP_PORT! HTTP_MAX_REQUESTS=!HTTP_MAX_REQUESTS! MYOS_HEADLESS=!MYOS_HEADLESS! /usr/bin/bash "!WSL_DIR!/tests/network/auto-network-test.sh" %*
set "RC=!errorlevel!"

echo.
if !RC! equ 0 (
    echo [auto-test] PASSED — see logs\auto-test.log
) else (
    echo [auto-test] FAILED ^(!RC!^) — see logs\auto-test.log and logs\qemu-serial.log
    echo            view-auto-test-log.bat
)
exit /b !RC!
