@echo off
setlocal
REM Double-click launcher: separations with the debug artifacts enabled.
REM Keep this file next to flux_separator.exe in the folder holding your WAVs.
cd /d "%~dp0"

if not exist "flux_separator.exe" (
    echo Error: flux_separator.exe was not found next to this file.
    pause
    exit /b 1
)

flux_separator.exe --debug %*

if errorlevel 1 pause
