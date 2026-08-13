@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ============================================================
echo   KnitOS: build + automated network test (CMD)
echo ============================================================
echo.
echo   Usage:
echo     build-test.bat              build (force network) + auto test
echo     build-test.bat rebuild      make clean + build + test
echo     build-test.bat --skip-build test only ^(no compile^)
echo.
echo   Logs: logs\auto-test.log  logs\qemu-serial.log
echo   View: view-auto-test-log.bat
echo.

set "BUILD_ARGS=force"
set "TEST_EXTRA="
if /i "%~1"=="rebuild" (
    call build.bat clean
    if errorlevel 1 exit /b 1
    set "BUILD_ARGS="
    shift
)
if /i "%~1"=="--skip-build" goto :test_only
if /i "%~1"=="-SkipBuild" goto :test_only

call build.bat !BUILD_ARGS!
if errorlevel 1 exit /b 1
set "TEST_EXTRA=--skip-build"

:test_only
call tests\auto-test.bat !TEST_EXTRA! %*
exit /b !errorlevel!
