@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Lumitool_Printsever_Setup_V3_10.ps1"
if %errorlevel% neq 0 (
  echo.
  echo Lumitool Printsever gap loi.
  echo Log: %%TEMP%%\Lumitool_Printsever_Setup_V3_10.log
  pause
)
endlocal
