@echo off
setlocal enabledelayedexpansion

set "MATCH_WAV="
set "BEST_LEN=0"

for %%I in ("%CD%") do set "CURR_DIR=%%~nxI"

for %%F in ("..\*.wav" "..\*.WAV") do (
    if exist "%%~fF" (
        call :CHECK_MATCH "%%~fF" "%%~nf"
    )
)

if defined MATCH_WAV (
    "%~dp0ct_analyze_headless.exe" --group --original "!MATCH_WAV!"
) else (
    "%~dp0ct_analyze_headless.exe" --group
)
pause
goto :EOF

:CHECK_MATCH
set "FILE_PATH=%~1"
set "WAV_NAME=%~2"
set "TMP_STR=%WAV_NAME%"
set "LEN=0"
for /L %%A in (1,1,256) do (
    if defined TMP_STR (
        set /A "LEN+=1"
        set "TMP_STR=!TMP_STR:~1!"
    )
)
if !LEN! LEQ 0 goto :EOF
call set "PREFIX=%%CURR_DIR:~0,!LEN!%%"
if /I "!PREFIX!"=="!WAV_NAME!" (
    if !LEN! GTR !BEST_LEN! (
        set "BEST_LEN=!LEN!"
        set "MATCH_WAV=!FILE_PATH!"
    )
)
goto :EOF
