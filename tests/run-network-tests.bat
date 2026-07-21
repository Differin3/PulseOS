@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.."

echo.
echo ============================================================
echo   MyOS — manual network tests (CMD)
echo ============================================================
echo.
echo  1. build.bat
echo  2. run-qemu-test.bat     ^(hostfwd 8080, serial TCP 4444^)
echo     OR run-qemu-ahci.bat  ^(writable disk^)
echo  3. In QEMU guest:
echo        autotest network 8080 16
echo     OR  dhcp
echo        write /www/test.txt hello-from-guest
echo        httpserver 8080 32
echo  4. While server runs ^(no shell prompt^), press a key here...
echo.
echo  Host tests target:
echo    tests\network\host-tests.bat 10.0.2.15 8080
echo    tests\network\host-tests.bat 127.0.0.1 8080   ^(with run-qemu-test hostfwd^)
echo.
pause

call tests\network\host-tests.bat
set "HOST_RC=!errorlevel!"

echo.
echo --- Serial log checks ^(after tests^) ---
call tests\network\check-serial-log.bat
set "LOG_RC=!errorlevel!"

echo.
if !HOST_RC! neq 0 (
    echo [RESULT] Host HTTP tests FAILED
    exit /b 1
)
if !LOG_RC! neq 0 (
    echo [RESULT] Serial log checks had warnings/failures
    exit /b 1
)
echo [RESULT] All automated checks passed
exit /b 0
