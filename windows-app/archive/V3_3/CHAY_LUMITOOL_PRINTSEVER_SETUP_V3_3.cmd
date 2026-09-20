@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Dang xin quyen Administrator...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo Lumitool Printsever Setup V3.3
echo Scan chay nen - giao dien khong bi treo khi quet IP.
echo.

powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Lumitool_Printsever_Setup_V3_3.ps1"

if %errorlevel% neq 0 (
    echo.
    echo =====================================================
    echo APP BI LOI - KHONG DONG CUA SO NAY
    echo Log:
    echo %%TEMP%%\Lumitool_Printsever_Setup_V3_3.log
    echo =====================================================
    echo.
    pause
)

endlocal
