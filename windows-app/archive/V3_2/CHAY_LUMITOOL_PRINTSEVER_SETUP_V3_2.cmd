@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Dang xin quyen Administrator...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo Lumitool Printsever Setup V3.2
echo Fix Standard TCP/IP RAW port 9101/9102/9103.
echo.

powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Lumitool_Printsever_Setup_V3_2.ps1"

if %errorlevel% neq 0 (
    echo.
    echo =====================================================
    echo APP BI LOI - KHONG DONG CUA SO NAY
    echo Log:
    echo %%TEMP%%\Lumitool_Printsever_Setup_V3_2.log
    echo =====================================================
    echo.
    pause
)

endlocal
