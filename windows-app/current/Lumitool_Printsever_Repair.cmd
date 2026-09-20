@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Lumitool_Fix_Printing_Status_V2.ps1"
if %errorlevel% neq 0 (
  echo.
  echo Sua trang thai job bi loi.
  pause
)
endlocal
