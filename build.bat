@echo off
rem SDRStudio - compilation Windows (MinGW-w64 / GCC + UCRT)
rem Prerequis : GCC installe (winget install BrechtSanders.WinLibs.POSIX.UCRT)
setlocal

set ROOT=%~dp0
cd /d "%ROOT%"

rem --- localisation du compilateur -------------------------------------------
set "GXX="
where g++ >nul 2>nul && set "GXX=g++"
if not defined GXX (
  for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%~d\mingw64\bin\g++.exe" (
      set "PATH=%%~d\mingw64\bin;%PATH%"
      set "GXX=%%~d\mingw64\bin\g++.exe"
    )
  )
)
if not defined GXX (
  echo [!] g++ introuvable.
  echo     Installer : winget install BrechtSanders.WinLibs.POSIX.UCRT
  exit /b 1
)
echo Compilateur : %GXX%

if not exist build mkdir build

set CXXFLAGS=-std=c++20 -O3 -march=native -ffast-math -Wno-cast-function-type -static -I core -I win
set LIBS=-lole32 -luuid -lgdi32 -luser32 -lwinmm

echo [1/2] Application SDRStudio.exe
"%GXX%" %CXXFLAGS% -o build\SDRStudio.exe ^
  core\dsp.cpp core\source.cpp core\recorder.cpp core\selftest.cpp ^
  win\rtl_win.cpp win\audio_wasapi.cpp win\main.cpp %LIBS%
if errorlevel 1 goto :error

echo [2/2] Banc d'essai sdr_selftest.exe
"%GXX%" %CXXFLAGS% -o build\sdr_selftest.exe ^
  core\dsp.cpp core\selftest.cpp tools\selftest_main.cpp
if errorlevel 1 goto :error

if exist vendor\rtlsdr (
  for /r "vendor\rtlsdr" %%f in (*.dll) do copy /y "%%f" build\ >nul
)

echo.
echo Termine : build\SDRStudio.exe
echo   SDRStudio.exe --probe      test de la cle
echo   SDRStudio.exe --selftest   mesures DSP
exit /b 0

:error
echo [!] Echec de compilation.
exit /b 1
