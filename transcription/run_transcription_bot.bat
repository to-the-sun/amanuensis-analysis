@echo off
setlocal enabledelayedexpansion

title Transcription Bot Launcher (Jules-harness)

echo ===================================================
echo     Discord Transcription Bot Launcher
echo     Google Cloud Project: Jules-harness (714089051017)
echo ===================================================
echo.

cd /d "%~dp0"

echo [1/4] Checking Python environment...
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Python is not installed or not in PATH.
    echo Please install Python 3.10+ and check "Add Python to PATH".
    echo.
    pause
    exit /b 1
)

echo [2/4] Verifying required Python packages...
python -m pip install -q discord.py requests pydub eng-to-ipa numpy send2trash >nul 2>&1

echo [3/4] Checking Google Cloud SDK & OAuth2 Status...
gcloud --version >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Google Cloud SDK detected.
    echo [INFO] Setting Google Cloud project to Jules-harness (714089051017)...
    call gcloud config set project 714089051017 >nul 2>&1

    :: Check if user is authenticated
    call gcloud auth print-access-token >nul 2>&1
    if %errorlevel% neq 0 (
        echo [INFO] OAuth2 authentication required. Launching Google login in browser...
        call gcloud auth login
    ) else (
        echo [INFO] OAuth2 authentication active.
    )
) else (
    echo [WARNING] Google Cloud SDK (gcloud) is not installed on your system.
    echo           If you wish to use Google Jules API, install gcloud or add
    echo           'jules_api_token' to credentials.json.
    echo.
    echo           Quick Install Command (PowerShell / CMD):
    echo           winget install Google.CloudSDK
    echo.
)

echo [4/4] Starting Transcription Bot (Aqua)...
echo ===================================================
echo.

python transcription_bot_aqua.py

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Transcription bot exited with an error code (%errorlevel%).
    echo.
)

pause
