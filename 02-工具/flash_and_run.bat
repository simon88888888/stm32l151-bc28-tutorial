@echo off
REM Build + flash + read the serial log. Safe to COPY INTO ANY PROJECT DIRECTORY:
REM double-click it there and it finds that project's Project.uvproj by itself.
REM Finds several? It lists them (newest build output first) and you pick one;
REM Enter = the newest. "-Yes" skips the question and always takes the newest.
REM (ASCII only - cmd reads .bat as GBK.)
REM -ExecutionPolicy Bypass is per-process only; it does NOT change any system setting.
REM Close Keil first: it holds the ST-Link and the flash step will abort.
setlocal

REM Prefer a .ps1 sitting next to this .bat; otherwise use the master copy.
REM %~dp0 is THIS .bat's folder - so a copied .bat still needs the absolute fallback.
set "PS1=%~dp0flash_and_run.ps1"
if not exist "%PS1%" set "PS1=D:\windows11-tools\stm32-bc28\flash_and_run.ps1"
if not exist "%PS1%" (
  echo Cannot find flash_and_run.ps1 - expected at:
  echo   %PS1%
  pause
  exit /b 1
)

REM Search from THIS .bat's folder (so a double-click finds the project wherever you
REM dropped it) UNLESS the first argument is a project path - i.e. something that is
REM not a -switch. Options like -Seconds 10 still let it search from here.
set "FIRST=%~1"
if "%FIRST%"=="" goto usedir
if "%FIRST:~0,1%"=="-" goto usedir
if "%FIRST:~0,1%"=="/" goto usedir
goto run
:usedir
cd /d "%~dp0"
:run

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
echo.
pause
