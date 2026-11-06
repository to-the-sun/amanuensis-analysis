@echo off
setlocal
REM Double-click launcher: atonal separations with raw and modified flux WAVs.
REM Keep this file next to flux_separator_atonal.exe in the folder holding your WAVs.
cd /d "%~dp0"

if not exist "flux_separator_atonal.exe" (
    echo Error: flux_separator_atonal.exe was not found next to this file.
    pause
    exit /b 1
)

flux_separator_atonal.exe --debug %*

if errorlevel 1 pause
