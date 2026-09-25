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

echo [3/4] Checking Google Cloud SDK and OAuth2 Status...
echo [3/4] Testing gcloud CLI availability...
call gcloud --version >nul 2>&1
if !errorlevel! equ 0 (
    echo [3/4] [SUCCESS] Google Cloud SDK detected.
    echo [3/4] Configuring Google Cloud project to Jules-harness [714089051017]...
    call gcloud config set project 714089051017 >nul 2>&1

    echo [3/4] Checking OAuth2 authentication token status...
    call gcloud auth print-access-token >nul 2>&1
    if !errorlevel! neq 0 (
        echo [3/4] [AUTH NEEDED] Active OAuth2 token not found. Launching Google login in browser...
        call gcloud auth login
    ) else (
        echo [3/4] [SUCCESS] Active Google OAuth2 authentication token confirmed.
    )
) else (
    echo [3/4] [WARNING] Google Cloud SDK [gcloud] is not installed or not in PATH.
    echo                 If using Google Jules API, install gcloud or specify 'jules_api_token' in credentials.json.
)
echo [3/4] Google Cloud check complete.
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
