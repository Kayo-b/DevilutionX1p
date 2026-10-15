@echo off
rem Diablo First-Person: sets the game up in this folder (install.ps1 does the work; README.txt explains).
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
if errorlevel 1 pause
