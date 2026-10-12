@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist "bin\kupyna-lab.exe" (
  echo Build the program first: powershell -File build.ps1
  pause
  exit /b 1
)
"bin\kupyna-lab.exe"
if errorlevel 1 pause
