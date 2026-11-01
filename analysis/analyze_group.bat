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

echo.
echo [debug] analyze.exe finished with exit code !ERRORLEVEL!

:: Copy song_builder.exe into the group [stems] folder, with verbose diagnostics
call :copy_song_builder

pause
goto :eof

:: ---------------------------------------------------------------
:: Copy song_builder.exe into [palettes]\[stems]
:: The original stays where it is. Every step prints what it is
:: looking at and what happened, so a failure can be traced from
:: the console output alone.
:: ---------------------------------------------------------------
:copy_song_builder
echo.
echo ============================================================
echo [helper] song_builder.exe copy
echo ============================================================
echo [helper] Current directory    : "%CD%"
echo [helper] Script directory     : "%~dp0"

set "SB_SRC="
set "SB_SRC_WHERE="
if exist "%~dp0song_builder.exe" (
    set "SB_SRC=%~dp0song_builder.exe"
    set "SB_SRC_WHERE=script folder"
) else if exist "song_builder.exe" (
    set "SB_SRC=%CD%\song_builder.exe"
    set "SB_SRC_WHERE=current directory"
)

if not defined SB_SRC goto :sb_not_found

set "SB_DEST_DIR=%CD%\[palettes]\[stems]"
set "SB_DEST=%SB_DEST_DIR%\song_builder.exe"

echo [helper] Source found in the !SB_SRC_WHERE!.
echo [helper] Source file          : "!SB_SRC!"
for %%A in ("!SB_SRC!") do echo [helper] Source size / date   : %%~zA bytes, %%~tA
for %%A in ("!SB_SRC!") do echo [helper] Source attributes    : [%%~aA]
echo [helper] Destination folder   : "!SB_DEST_DIR!"
echo [helper] Destination file     : "!SB_DEST!"
echo [helper] This is a copy - the original stays where it is.

if exist "!SB_DEST_DIR!\" goto :sb_dest_ready
echo [helper] Destination folder is missing - creating it...
mkdir "!SB_DEST_DIR!"
set "SB_MKDIR_ERR=!ERRORLEVEL!"
echo [helper] mkdir exit code      : !SB_MKDIR_ERR!
if exist "!SB_DEST_DIR!\" goto :sb_dest_ready
echo [helper] ERROR: could not create "!SB_DEST_DIR!" - no copy was attempted.
echo [helper] Check permissions, a read-only drive, or the Windows path length limit.
goto :sb_list_dest

:sb_dest_ready
echo [helper] Destination folder exists.
echo [helper] Testing whether the destination folder accepts new files...
set "SB_PROBE=!SB_DEST_DIR!\_write_probe_%RANDOM%.tmp"
echo probe>"!SB_PROBE!" 2>nul
if not exist "!SB_PROBE!" goto :sb_probe_failed
del "!SB_PROBE!" >nul 2>nul
echo [helper] Write test           : OK
goto :sb_copy

:sb_probe_failed
echo [helper] Write test           : FAILED - the folder does not accept new files.
echo [helper] Likely reasons: read-only folder or drive, permission denied,
echo [helper] antivirus protection, or a full disk. Trying the copy anyway...

:sb_copy
echo [helper] Running: copy /y "!SB_SRC!" "!SB_DEST!"
copy /y "!SB_SRC!" "!SB_DEST!"
set "SB_COPY_ERR=!ERRORLEVEL!"
echo [helper] copy exit code       : !SB_COPY_ERR!

if exist "!SB_DEST!" (
    echo [helper] Destination exists  : YES
) else (
    echo [helper] Destination exists  : NO
)

if not exist "!SB_DEST!" goto :sb_copy_failed

for %%A in ("!SB_DEST!") do echo [helper] Copied size / date   : %%~zA bytes, %%~tA
set "SB_SRC_SIZE=0"
set "SB_DST_SIZE=0"
for %%A in ("!SB_SRC!") do set "SB_SRC_SIZE=%%~zA"
for %%A in ("!SB_DEST!") do set "SB_DST_SIZE=%%~zA"
if "!SB_SRC_SIZE!"=="!SB_DST_SIZE!" (
    echo [helper] Size check           : matches the source, the copy looks complete.
) else (
    echo [helper] WARNING: size mismatch - source !SB_SRC_SIZE! bytes vs destination !SB_DST_SIZE! bytes.
    echo [helper] The copy is incomplete or the destination file was replaced while copying.
)
echo [helper] SUCCESS: song_builder.exe copied to "!SB_DEST_DIR!"
echo [helper] The original is still at "!SB_SRC!"
echo [helper] Contents of the destination folder:
dir /b "!SB_DEST_DIR!" 2>nul
goto :sb_done

:sb_copy_failed
echo [helper] FAILED: the copy did not create "!SB_DEST!".
echo [helper] The source is still present: "!SB_SRC!"
echo [helper] Source attributes after the failed copy:
attrib "!SB_SRC!"
echo [helper] Usual reasons at this exact step:
echo [helper]   - destination folder is read-only, deleted mid-run, or locked
echo [helper]   - antivirus or another program is scanning song_builder.exe
echo [helper]   - destination drive is full or the path is over the length limit
echo [helper]   - a folder named song_builder.exe already exists at the destination
echo [helper] Contents of the destination folder:
dir /b "!SB_DEST_DIR!" 2>nul
echo [helper] Any song_builder.exe already present under the current folder:
dir /b /s "%CD%\song_builder.exe" 2>nul
goto :sb_done

:sb_list_dest
echo [helper] Contents of the destination folder, if it exists:
dir /b "!SB_DEST_DIR!" 2>nul
goto :sb_done

:sb_not_found
echo [helper] WARNING: song_builder.exe was not found, so nothing was copied.
echo [helper] Looked for: "%~dp0song_builder.exe"
echo [helper] Looked for: "%CD%\song_builder.exe"
echo [helper] Files matching song_builder* next to this script:
dir /b "%~dp0song_builder*" 2>nul
echo [helper] Files matching song_builder* in the current directory:
dir /b "song_builder*" 2>nul
goto :sb_done

:sb_done
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
