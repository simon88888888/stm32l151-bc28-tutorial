@echo off
REM Read the board's USB1 log (CH340 @ 9600 8-N-1). Safe to copy anywhere.
REM (ASCII only - cmd reads .bat as GBK.)
REM -ExecutionPolicy Bypass is per-process only; it does NOT change any system setting.
setlocal

REM Prefer a .ps1 sitting next to this .bat; otherwise use the master copy.
REM %~dp0 is THIS .bat's folder, so a copied .bat still needs the absolute fallback.
set "PS1=%~dp0read_com.ps1"
if not exist "%PS1%" set "PS1=D:\windows11-tools\stm32-bc28\read_com.ps1"
if not exist "%PS1%" (
  echo Cannot find read_com.ps1 - expected at:
  echo   %PS1%
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
echo.
pause
