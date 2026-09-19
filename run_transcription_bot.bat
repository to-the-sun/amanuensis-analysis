@echo off
title Transcription Bot Launcher
cd /d "%~dp0"

echo ===================================================
echo   Checking and Installing Required Dependencies...
echo ===================================================
python -m pip install --upgrade pip
python -m pip install torch sentencepiece huggingface_hub discord.py requests numpy eng-to-ipa nltk syllables SoundsLike pronouncing pydub send2trash aiohttp beautifulsoup4 soundcard soundfile

echo.
echo ===================================================
echo   Starting Transcription Bot...
echo ===================================================
if exist "transcription\transcription_bot_aqua.py" (
    python transcription\transcription_bot_aqua.py
) else if exist "transcription_bot_aqua.py" (
    python transcription_bot_aqua.py
) else (
    echo Error: Could not find transcription_bot_aqua.py script.
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Bot stopped with an error code: %ERRORLEVEL%
)

pause
