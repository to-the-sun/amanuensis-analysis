@echo off
setlocal enabledelayedexpansion

title Transcription Bot Launcher

echo ===================================================
echo     Discord Transcription Bot Launcher
echo ===================================================
echo.

:: Change directory to script directory
cd /d "%~dp0"

echo [1/3] Checking Python environment...
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Python is not installed or not in PATH.
    echo Please install Python 3.10+ and check "Add Python to PATH".
    echo.
    pause
    exit /b 1
)

echo [2/3] Verifying and installing required Python packages...
python -m pip install -q discord.py requests pydub eng-to-ipa numpy send2trash >nul 2>&1

echo [3/3] Checking Google Cloud / OAuth2 status...
gcloud --version >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Google Cloud SDK detected.
) else (
    echo [INFO] Google Cloud SDK not detected.
    echo        If using Google Jules API, ensure 'jules_api_token' or 'jules_refresh_token'
    echo        is configured in credentials.json.
)

echo.
echo ===================================================
echo Starting Transcription Bot (Aqua)...
echo ===================================================
echo.

python transcription_bot_aqua.py

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Transcription bot exited with an error code (%errorlevel%).
    echo.
)

pause
