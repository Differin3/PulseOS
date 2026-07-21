@echo off
cd /d "%~dp0"

echo Creating directory structure...
if not exist "kernel\drivers\storage" mkdir "kernel\drivers\storage"
if not exist "kernel\drivers\input" mkdir "kernel\drivers\input"
if not exist "kernel\drivers\video" mkdir "kernel\drivers\video"
if not exist "kernel\drivers\pci" mkdir "kernel\drivers\pci"

echo Moving storage drivers...
move kernel\ahci.cpp kernel\drivers\storage\ >nul 2>&1
move kernel\ahci.h kernel\drivers\storage\ >nul 2>&1
move kernel\nvme.cpp kernel\drivers\storage\ >nul 2>&1
move kernel\nvme.h kernel\drivers\storage\ >nul 2>&1
move kernel\ata.cpp kernel\drivers\storage\ >nul 2>&1
move kernel\ata.h kernel\drivers\storage\ >nul 2>&1
move kernel\disk_manager.cpp kernel\drivers\storage\ >nul 2>&1
move kernel\disk_manager.h kernel\drivers\storage\ >nul 2>&1

echo Moving input drivers...
move kernel\keyboard.cpp kernel\drivers\input\ >nul 2>&1
move kernel\keyboard.h kernel\drivers\input\ >nul 2>&1

echo Moving video drivers...
move kernel\terminal.cpp kernel\drivers\video\ >nul 2>&1
move kernel\terminal.h kernel\drivers\video\ >nul 2>&1

echo Moving PCI driver...
move kernel\pci.cpp kernel\drivers\pci\ >nul 2>&1
move kernel\pci.h kernel\drivers\pci\ >nul 2>&1

echo Done!
pause
