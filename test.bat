@echo off
rem Shortcut: fully automated network test (build QEMU inside WSL, logs to logs\)
cd /d "%~dp0"
call tests\auto-test.bat %*
