@if not "%~1"=="__KEEPOPEN__" (
    cmd /k ""%~f0" __KEEPOPEN__ %*"
    exit /b
)

@echo off
setlocal enabledelayedexpansion

title Transcription Bot Launcher (Jules-harness)

echo ===================================================
echo     Discord Transcription Bot Launcher
echo     Google Cloud Project: Jules-harness [714089051017]
echo ===================================================
echo.

cd /d "%~dp0"

echo [1/4] Checking Python environment...
python --version >nul 2>&1
if !errorlevel! neq 0 (
    echo [ERROR] Python is not installed or not added to PATH.
    echo Please install Python 3.10+ and make sure "Add Python to PATH" is checked.
    goto :end
)
echo [1/4] Python environment OK.
echo.

echo [2/4] Verifying required Python packages...
python -m pip install discord.py requests pydub eng-to-ipa numpy send2trash
if !errorlevel! neq 0 (
    echo [WARNING] Some Python dependencies failed to install. Continuing...
)
echo [2/4] Python packages verified.
echo.

echo [3/4] Checking Google Jules API credentials...
if defined JULES_API_KEY (
    echo [3/4] [SUCCESS] JULES_API_KEY environment variable detected.
) else (
    if exist credentials.json (
        echo [3/4] [SUCCESS] Local credentials.json detected.
    ) else (
        echo [3/4] [NOTICE] No JULES_API_KEY environment variable or credentials.json found.
        echo                 Generate an API key at jules.google.com/settings and save 'jules_api_key' in credentials.json.
    )
)
echo [3/4] Jules API check complete.
echo.

echo [4/4] Starting Transcription Bot [Aqua]...
echo ===================================================
echo.

python transcription_bot_aqua.py

if !errorlevel! neq 0 (
    echo.
    echo ===================================================
    echo [ERROR] Transcription bot stopped with error code !errorlevel!.
    echo ===================================================
    echo.
) else (
    echo.
    echo Transcription bot finished execution.
    echo.
)

:end
echo ===================================================
echo Console window remaining open indefinitely.
echo You can inspect logs above or close this window.
echo ===================================================
pause
