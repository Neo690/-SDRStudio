@echo off
rem SDR Studio - construit l'installateur Windows.
rem  1. build.bat (application + auto-test)
rem  2. make-assets.ps1 (icone et images de l'assistant)
rem  3. ISCC sdrstudio.iss -> dist\SDRStudio-1.0.0-setup.exe
rem
rem Prerequis : MinGW-w64 pour build.bat, Inno Setup 6.6+ :
rem   winget install -e --id JRSoftware.InnoSetup
setlocal
cd /d "%~dp0"

if exist "..\build\SDRStudio.exe" (
  echo [1/3] build\SDRStudio.exe deja compile
) else (
  echo [1/3] Compilation de l'application...
  call "..\build.bat"
  if errorlevel 1 goto :error
)

echo [2/3] Generation de l'icone et des images de l'assistant...
powershell -NoProfile -ExecutionPolicy Bypass -File "make-assets.ps1"
if errorlevel 1 goto :error

echo [3/3] Compilation de l'installateur...
set "ISCC="
for %%p in (
  "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
  "%ProgramFiles%\Inno Setup 6\ISCC.exe"
  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
) do if not defined ISCC if exist %%p set "ISCC=%%~p"
if not defined ISCC (
  echo [!] ISCC.exe introuvable.
  echo     Installer Inno Setup 6.6 ou plus recent :
  echo       winget install -e --id JRSoftware.InnoSetup
  goto :error
)
"%ISCC%" sdrstudio.iss
if errorlevel 1 goto :error

echo.
echo Termine :
for %%f in ("dist\*.exe") do echo   %%~ff  (%%~zf octets)
exit /b 0

:error
echo [!] Echec de la construction de l'installateur.
exit /b 1
