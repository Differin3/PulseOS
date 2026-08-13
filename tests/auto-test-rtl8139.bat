@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.."

rem RTL8139 regression path for the modern network stack (fallback NIC).
set "MYOS_NIC=rtl8139"
call "%~dp0auto-test.bat" --rtl8139 %*
exit /b %ERRORLEVEL%
