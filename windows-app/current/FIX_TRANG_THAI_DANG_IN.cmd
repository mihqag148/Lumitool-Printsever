@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Dang xin quyen Administrator...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Lumitool_Fix_Printing_Status_V2.ps1"

if %errorlevel% neq 0 (
    echo.
    echo FIX QUEUE BI LOI.
    pause
)

endlocal
