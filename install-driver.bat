@echo off
rem SDRStudio - installation du pilote WinUSB pour la cle RTL-SDR (Zadig)
rem 1. Ce script ouvre Zadig en administrateur (via l'invite UAC).
rem 2. Dans Zadig : selectionner "RTL2832U", pilote "WinUSB", cliquer "Install Driver".
cd /d "%~dp0vendor\tools"
if not exist zadig-2.9.exe (
  echo [!] zadig-2.9.exe absent de vendor\tools
  pause
  exit /b 1
)
start "" zadig-2.9.exe
