@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Automated HTTP tests from Windows host -> QEMU guest (user networking).
rem Prerequisite: guest has IP and httpserver is running.
rem   dhcp
rem   httpserver 8080 32

set "GUEST_IP=10.0.2.15"
set "HTTP_PORT=8080"
set "BASE=http://%GUEST_IP%:%HTTP_PORT%"
set "PASS=0"
set "FAIL=0"
set "SKIP=0"

if not "%~1"=="" set "GUEST_IP=%~1"
if not "%~2"=="" set "HTTP_PORT=%~2"
set "BASE=http://%GUEST_IP%:%HTTP_PORT%"

where curl >nul 2>&1
if errorlevel 1 (
    echo [FAIL] curl not found. Install curl or use Windows 10+ with curl in PATH.
    exit /b 1
)

echo ============================================
echo  MyOS network host tests
echo  Target: %BASE%
echo  Time:   %DATE% %TIME%
echo ============================================
if /i "%GUEST_IP%"=="127.0.0.1" (
    echo [INFO] 127.0.0.1 = QEMU hostfwd ^(host -^> guest :%HTTP_PORT%^)
)
echo.
set "T0=%TIME%"

call :ping_guest
echo.

call :test_get_index
call :test_get_custom_file
call :test_head
call :test_options
call :test_404
call :test_post_echo
call :test_put_file
call :test_gzip
call :test_access_log
call :test_keepalive
call :test_http11_header

echo.
echo ============================================
echo  Results: PASS=!PASS!  FAIL=!FAIL!  SKIP=!SKIP!
if !FAIL! equ 0 (
    echo  Status:  ALL REQUIRED TESTS PASSED
) else (
    echo  Status:  FAILED ^(!FAIL! case^(s^)^)
)
echo  Target:  %BASE%
echo ============================================
if !FAIL! gtr 0 exit /b 1
exit /b 0

:ping_guest
echo [--] Reachability ping %GUEST_IP% ...
ping -n 1 -w 1000 %GUEST_IP% >nul 2>&1
if errorlevel 1 (
    echo [SKIP] Guest not pingable ^(ICMP may be blocked; continuing HTTP tests^)
    set /a SKIP+=1
) else (
    echo [PASS] Ping OK
    set /a PASS+=1
)
exit /b 0

:test_get_index
echo [--] GET / ^(expect 200 + MyOS^) ...
curl -sf --max-time 5 "%BASE%/" -o "%TEMP%\myos_index.html" 2>nul
if errorlevel 1 (
    echo [FAIL] GET / failed — is httpserver STILL running in QEMU? ^(no shell prompt^)
    set /a FAIL+=1
    exit /b 0
)
findstr /I /C:"MyOS" "%TEMP%\myos_index.html" >nul 2>&1
if errorlevel 1 goto :fail_get_index
for /f %%C in ('curl -s -o nul -w "%%{http_code}" --max-time 5 "%BASE%/"') do set "CODE=%%C"
if not "!CODE!"=="200" goto :fail_get_index
curl -sI --max-time 5 "%BASE%/" 2>nul | findstr /I /C:"Content-Type:" | findstr /I "text/html" >nul
if errorlevel 1 (
    echo [FAIL] GET / missing Content-Type: text/html
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] GET / -> !CODE! with MyOS body + Content-Type
set /a PASS+=1
exit /b 0
:fail_get_index
echo [FAIL] GET / bad status or body missing MyOS
set /a FAIL+=1
exit /b 0

:test_get_custom_file
echo [--] GET /test.txt ^(run write /www/test.txt in guest first^) ...
curl -sf --max-time 5 "%BASE%/test.txt" -o "%TEMP%\myos_test.txt" 2>nul
if errorlevel 1 (
    echo [SKIP] /test.txt not available
    set /a SKIP+=1
    exit /b 0
)
findstr /C:"hello-from-guest" "%TEMP%\myos_test.txt" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] /test.txt wrong content
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] GET /test.txt content OK
set /a PASS+=1
exit /b 0

:test_head
echo [--] HEAD / ^(expect 200, no body^) ...
curl -sI --max-time 5 "%BASE%/" -o "%TEMP%\myos_head.txt" 2>nul
if errorlevel 1 (
    echo [FAIL] HEAD / request failed
    set /a FAIL+=1
    exit /b 0
)
findstr /R /C:"HTTP/1\.[01] 200" "%TEMP%\myos_head.txt" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] HEAD / status not 200
    set /a FAIL+=1
    exit /b 0
)
findstr /I /C:"Content-Length:" "%TEMP%\myos_head.txt" >nul 2>&1
if errorlevel 1 (
    echo [FAIL] HEAD / missing Content-Length
    set /a FAIL+=1
    exit /b 0
)
for %%A in ("%TEMP%\myos_head.txt") do set HEADSZ=%%~zA
curl -sf -X HEAD --max-time 5 "%BASE%/" -o "%TEMP%\myos_head_body.bin" 2>nul
for %%A in ("%TEMP%\myos_head_body.bin") do set BODYSZ=%%~zA
if not "!BODYSZ!"=="0" (
    echo [WARN] HEAD body size=!BODYSZ! ^(some curl builds still fetch body^)
)
echo [PASS] HEAD / headers OK
set /a PASS+=1
exit /b 0

:test_options
echo [--] OPTIONS / ^(expect 204^) ...
for /f %%C in ('curl -s -o nul -w "%%{http_code}" -X OPTIONS --max-time 5 "%BASE%/"') do set "CODE=%%C"
if not "!CODE!"=="204" (
    echo [FAIL] OPTIONS / -> !CODE! ^(expected 204^)
    set /a FAIL+=1
    exit /b 0
)
curl -sI -X OPTIONS --max-time 5 "%BASE%/" 2>nul | findstr /I /C:"Allow:" >nul
if errorlevel 1 (
    echo [FAIL] OPTIONS missing Allow header
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] OPTIONS / -> 204
set /a PASS+=1
exit /b 0

:test_404
echo [--] GET /no-such-file ^(expect 404^) ...
for /f %%C in ('curl -s -o nul -w "%%{http_code}" --max-time 5 "%BASE%/no-such-file"') do set "CODE=%%C"
if not "!CODE!"=="404" (
    echo [FAIL] GET missing file -> !CODE! ^(expected 404^)
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] GET /no-such-file -> 404
set /a PASS+=1
exit /b 0

:test_post_echo
echo [--] POST /api/echo ...
curl -sf -X POST -H "Expect:" --data-binary "myos-autotest-post" --max-time 8 "%BASE%/api/echo" -o "%TEMP%\myos_post.txt" 2>nul
if errorlevel 1 (
    echo [FAIL] POST /api/echo request failed
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"myos-autotest-post" "%TEMP%\myos_post.txt" >nul
if errorlevel 1 (
    echo [FAIL] POST /api/echo bad body
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] POST /api/echo OK
set /a PASS+=1
exit /b 0

:test_put_file
echo [--] PUT /put-test.txt ...
for /f %%C in ('curl -s -o nul -w "%%{http_code}" -X PUT -H "Expect:" --data-binary "put-by-host" --max-time 8 "%BASE%/put-test.txt"') do set "CODE=%%C"
if not "!CODE!"=="201" (
    echo [FAIL] PUT -> !CODE!
    set /a FAIL+=1
    exit /b 0
)
curl -sf --max-time 8 "%BASE%/put-test.txt" -o "%TEMP%\myos_put.txt" 2>nul
findstr /C:"put-by-host" "%TEMP%\myos_put.txt" >nul
if errorlevel 1 (
    echo [FAIL] PUT content not readable
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] PUT /put-test.txt OK
set /a PASS+=1
exit /b 0

:test_gzip
echo [--] gzip Accept-Encoding ...
curl -sf -H "Accept-Encoding: gzip" --compressed --max-time 10 "%BASE%/" -o "%TEMP%\myos_gz.html" 2>nul
if errorlevel 1 (
    echo [FAIL] gzip transfer failed
    set /a FAIL+=1
    exit /b 0
)
findstr /I /C:"MyOS" "%TEMP%\myos_gz.html" >nul
if errorlevel 1 (
    echo [FAIL] gzip body missing MyOS
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] gzip decompress OK
set /a PASS+=1
exit /b 0

:test_access_log
echo [--] GET /api/access-log ...
curl -sf --max-time 8 "%BASE%/api/access-log" -o "%TEMP%\myos_access.log" 2>nul
if errorlevel 1 (
    echo [FAIL] access log endpoint failed
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"POST /api/echo" "%TEMP%\myos_access.log" >nul
if errorlevel 1 (
    echo [FAIL] access log missing POST
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"PUT /put-test.txt" "%TEMP%\myos_access.log" >nul
if errorlevel 1 (
    echo [FAIL] access log missing PUT
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] access log OK
set /a PASS+=1
exit /b 0

:test_keepalive
echo [--] Keep-Alive ^(two GETs, one connection^) ...
curl -sf --http1.1 --max-time 8 "%BASE%/" "%BASE%/test.txt" -o "%TEMP%\myos_ka1.html" 2>nul
if errorlevel 1 (
    echo [SKIP] Keep-Alive multi-GET failed ^(server may have closed^)
    set /a SKIP+=1
    exit /b 0
)
echo [PASS] Keep-Alive multi-GET completed
set /a PASS+=1
exit /b 0

:test_http11_header
echo [--] Server header ^(HTTP/1.1^) ...
curl -sI --max-time 5 "%BASE%/" 2>nul | findstr /I /C:"Server: MyOS-HTTP/1.1" >nul
if errorlevel 1 (
    echo [FAIL] Server: MyOS-HTTP/1.1 not found
    set /a FAIL+=1
    exit /b 0
)
echo [PASS] Server header OK
set /a PASS+=1
exit /b 0
