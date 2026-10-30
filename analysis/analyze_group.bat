@echo off
setlocal enabledelayedexpansion

:: Get the current folder name and parent folder name
for %%I in ("%CD%") do set "CURR_DIR_NAME=%%~nxI"
for %%I in ("%CD%\..") do set "PARENT_DIR_NAME=%%~nxI"

set "MATCHED_WAV="
set "BEST_LEN=0"

:: Check parent directory for matching WAV files
if exist "..\*.wav" (
    for %%F in ("..\*.wav") do (
        call :check_wav "..\%%~nxF" "%%~nF" "!CURR_DIR_NAME!"
    )
)

:: If not found in parent directory, check two levels up
if not defined MATCHED_WAV (
    if exist "..\..\*.wav" (
        for %%F in ("..\..\*.wav") do (
            call :check_wav "..\..\%%~nxF" "%%~nF" "!PARENT_DIR_NAME!"
        )
    )
)

if defined MATCHED_WAV (
    "%~dp0analyze.exe" --group --original "!MATCHED_WAV!" %*
) else (
    "%~dp0analyze.exe" --group %*
)

if exist "%~dp0song_builder.exe" (
    if not exist "[palettes]\[stems]" mkdir "[palettes]\[stems]"
    move /y "%~dp0song_builder.exe" "[palettes]\[stems]\" >nul
) else if exist "song_builder.exe" (
    if not exist "[palettes]\[stems]" mkdir "[palettes]\[stems]"
    move /y "song_builder.exe" "[palettes]\[stems]\" >nul
)

pause
goto :eof

:check_wav
set "WAV_PATH=%~1"
set "WAV_NAME=%~2"
set "TARGET_DIR=%~3"
if not defined TARGET_DIR set "TARGET_DIR=!CURR_DIR_NAME!"

:: Calculate character length of WAV_NAME
set "STR=%WAV_NAME%"
set "WAV_LEN=0"

:len_loop
if defined STR (
    set "STR=%STR:~1%"
    set /a WAV_LEN+=1
    goto :len_loop
)

if %WAV_LEN% gtr 0 (
    set "PREFIX=!TARGET_DIR:~0,%WAV_LEN%!"
    if /i "!PREFIX!"=="%WAV_NAME%" (
        if %WAV_LEN% gtr %BEST_LEN% (
            set "BEST_LEN=%WAV_LEN%"
            set "MATCHED_WAV=%WAV_PATH%"
        )
    )
)
goto :eof
