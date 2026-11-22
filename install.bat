@echo off
REM ============================================================================
REM  install.bat - Install font-effect-tools into OBS Studio (Windows x64).
REM
REM  Follows the official OBS plugin-location guidance:
REM    https://obsproject.com/kb/legacy-plugin-developer-info
REM
REM  What it does:
REM    1. Finds OBS (argument > registry "HKLM\SOFTWARE\OBS Studio" >
REM       %ProgramFiles%\obs-studio) and reads the version of obs64.exe.
REM    2. Refuses to run while OBS is running.
REM    3. Removes old copies from the legacy Program Files location:
REM         <obs>\obs-plugins\64bit\font-effect-tools.dll / .pdb
REM         <obs>\data\obs-plugins\font-effect-tools\
REM    4. Installs into the location that OBS version actually loads:
REM         OBS 33+ : %ProgramData%\obs-studio\plugins\font-effect-tools\
REM                     font-effect-tools.dll, data\
REM         OBS 32- : %ProgramData%\obs-studio\plugins\font-effect-tools\
REM                     bin\64bit\font-effect-tools.dll, data\
REM         Portable OBS 33+ : <obs>\plugins\font-effect-tools\ (flat)
REM       The plugin folder is mirrored, so files from older versions are
REM       removed.
REM
REM  Usage:
REM    install.bat                 Install (asks for admin rights via UAC)
REM    install.bat "D:\obs-studio" Install, using this OBS folder
REM    install.bat /check [dir]    Only show what would be done (no admin)
REM    /nopause                    Do not wait for a key at the end, and do
REM                                not self-elevate (for use from scripts)
REM ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "PLUGIN=font-effect-tools"
set "SRC=%~dp0%PLUGIN%"
set "CHECK_ONLY=0"
set "NOPAUSE=0"
set "ELEVATED=0"
set "OBS_DIR="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="/check"   (set "CHECK_ONLY=1" & shift & goto :parse_args)
if /i "%~1"=="/nopause" (set "NOPAUSE=1" & shift & goto :parse_args)
if /i "%~1"=="/elevated" (set "ELEVATED=1" & shift & goto :parse_args)
set "OBS_DIR=%~1"
shift
goto :parse_args
:args_done

echo === %PLUGIN% installer ===

REM --- package sanity -------------------------------------------------------
if not exist "%SRC%\%PLUGIN%.dll" (
    echo [error] "%SRC%\%PLUGIN%.dll" not found.
    echo         Extract the whole ZIP first and run install.bat from inside it.
    goto :fail
)
if not exist "%SRC%\data\locale\ja-JP.ini" (
    echo [error] "%SRC%\data\" is incomplete. Re-extract the ZIP.
    goto :fail
)

REM --- 1. locate OBS --------------------------------------------------------
set "OBS_FROM=argument"
if defined OBS_DIR goto :have_obs_dir
set "OBS_FROM=registry"
for /f "tokens=2,*" %%A in ('reg query "HKLM\SOFTWARE\OBS Studio" /ve 2^>nul ^| find "REG_SZ"') do set "OBS_DIR=%%B"
if defined OBS_DIR goto :have_obs_dir
set "OBS_FROM=default path"
set "OBS_DIR=%ProgramFiles%\obs-studio"
:have_obs_dir
if "%OBS_DIR:~-1%"=="\" set "OBS_DIR=%OBS_DIR:~0,-1%"

if not exist "%OBS_DIR%\bin\64bit\obs64.exe" (
    echo [error] OBS not found at "%OBS_DIR%" ^(from %OBS_FROM%^).
    echo         Pass the OBS folder explicitly, e.g.:
    echo           install.bat "D:\Apps\obs-studio"
    goto :fail
)

set "OBS_MAJOR="
set "OBS_VERSION="
for /f "tokens=1,2" %%A in ('powershell -NoProfile -Command "$v=(Get-Item -LiteralPath (Join-Path $env:OBS_DIR 'bin\64bit\obs64.exe')).VersionInfo; Write-Output ('{0} {1}' -f $v.FileMajorPart, $v.ProductVersion)"') do (
    set "OBS_MAJOR=%%A"
    set "OBS_VERSION=%%B"
)
set /a OBS_MAJOR=OBS_MAJOR+0 >nul 2>&1
if %OBS_MAJOR% LSS 1 (
    echo [error] Could not read the version of "%OBS_DIR%\bin\64bit\obs64.exe".
    goto :fail
)

set "PORTABLE=0"
if exist "%OBS_DIR%\obs_portable_mode"     set "PORTABLE=1"
if exist "%OBS_DIR%\portable_mode.txt"     set "PORTABLE=1"
if exist "%OBS_DIR%\obs_portable_mode.txt" set "PORTABLE=1"

REM --- 2. decide destination + layout ---------------------------------------
REM  LAYOUT=flat  : <dest>\font-effect-tools.dll + <dest>\data\   (OBS 33+)
REM  LAYOUT=bin64 : <dest>\bin\64bit\font-effect-tools.dll + <dest>\data\
if "%PORTABLE%"=="1" (
    if %OBS_MAJOR% LSS 33 (
        echo [error] Portable OBS %OBS_VERSION% is not supported by this installer.
        echo         OBS 32 portable only loads plugins from its own obs-plugins
        echo         folder. Update OBS to 33 or later, or install manually.
        goto :fail
    )
    set "DEST=%OBS_DIR%\plugins\%PLUGIN%"
    set "LAYOUT=flat"
) else (
    set "DEST=%ProgramData%\obs-studio\plugins\%PLUGIN%"
    if %OBS_MAJOR% GEQ 33 (set "LAYOUT=flat") else (set "LAYOUT=bin64")
)

set "LEGACY_DLL=%OBS_DIR%\obs-plugins\64bit\%PLUGIN%.dll"
set "LEGACY_PDB=%OBS_DIR%\obs-plugins\64bit\%PLUGIN%.pdb"
set "LEGACY_DATA=%OBS_DIR%\data\obs-plugins\%PLUGIN%"

echo   OBS folder   : %OBS_DIR%  ^(from %OBS_FROM%^)
echo   OBS version  : %OBS_VERSION%  portable=%PORTABLE%
echo   Install to   : %DEST%  ^(layout %LAYOUT%^)
set "HAS_LEGACY=0"
if exist "%LEGACY_DLL%"  (echo   Legacy copy  : %LEGACY_DLL%  ^(will be removed^)  & set "HAS_LEGACY=1")
if exist "%LEGACY_PDB%"  (echo   Legacy copy  : %LEGACY_PDB%  ^(will be removed^)  & set "HAS_LEGACY=1")
if exist "%LEGACY_DATA%" (echo   Legacy copy  : %LEGACY_DATA%\  ^(will be removed^) & set "HAS_LEGACY=1")
if "%HAS_LEGACY%"=="0" echo   Legacy copy  : none

REM --- 3. OBS must be closed -----------------------------------------------
set "OBS_RUNNING=0"
tasklist /FI "IMAGENAME eq obs64.exe" /NH 2>nul | find /I "obs64.exe" >nul && set "OBS_RUNNING=1"
if "%OBS_RUNNING%"=="1" echo   OBS running  : YES

if "%CHECK_ONLY%"=="1" (
    echo.
    echo [check] Nothing was changed.
    goto :ok
)

if "%OBS_RUNNING%"=="1" (
    echo [error] OBS is running. Close OBS completely and run install.bat again.
    goto :fail
)

REM --- 4. admin rights (ProgramData + Program Files are protected) --------
REM  fltmc needs admin and, unlike "net session", does not depend on the
REM  Server (LanmanServer) service.
fltmc >nul 2>&1
if not errorlevel 1 goto :is_admin

REM Not elevated. With /nopause (called from scripts such as build.bat) we
REM must not spawn a separate window, so just fail. Otherwise relaunch this
REM script elevated via UAC; the new window shows the result.
if "%NOPAUSE%"=="1" goto :need_admin
if "%ELEVATED%"=="1" goto :need_admin
echo.
echo Administrator rights are needed. Requesting them now (UAC prompt)...
echo The installation continues in a new window.
set "SELF=%~f0"
set "SELF_ARG="
if "%OBS_FROM%"=="argument" set "SELF_ARG=%OBS_DIR%"
powershell -NoProfile -Command "$a = @('/elevated'); if ($env:SELF_ARG) { $a += ('\"' + $env:SELF_ARG + '\"') }; try { Start-Process -FilePath $env:SELF -ArgumentList $a -Verb RunAs -ErrorAction Stop; exit 0 } catch { exit 1 }"
if errorlevel 1 (
    echo [error] Administrator rights were not granted ^(UAC was cancelled^).
    goto :need_admin
)
endlocal
exit /b 0

:need_admin
echo [error] Administrator privileges are required.
echo         Right-click install.bat and choose "Run as administrator".
goto :fail

:is_admin

REM --- 5. remove legacy copies ---------------------------------------------
if exist "%LEGACY_DLL%"  del /f /q "%LEGACY_DLL%"
if exist "%LEGACY_PDB%"  del /f /q "%LEGACY_PDB%"
if exist "%LEGACY_DATA%" rmdir /s /q "%LEGACY_DATA%"
if exist "%LEGACY_DLL%" (
    echo [error] Could not remove "%LEGACY_DLL%" ^(in use?^).
    goto :fail
)
if exist "%LEGACY_DATA%" (
    echo [error] Could not remove "%LEGACY_DATA%".
    goto :fail
)

REM --- 6. install -----------------------------------------------------------
if "%LAYOUT%"=="flat" (
    REM Mirror the whole plugin folder: also drops an old bin\ tree.
    robocopy "%SRC%" "%DEST%" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP /NFL >nul
    if errorlevel 8 goto :copy_fail
) else (
    robocopy "%SRC%\data" "%DEST%\data" /MIR /R:1 /W:1 /NJH /NJS /NDL /NP /NFL >nul
    if errorlevel 8 goto :copy_fail
    if exist "%DEST%\bin" rmdir /s /q "%DEST%\bin"
    robocopy "%SRC%" "%DEST%\bin\64bit" %PLUGIN%.dll %PLUGIN%.pdb /R:1 /W:1 /NJH /NJS /NDL /NP /NFL >nul
    if errorlevel 8 goto :copy_fail
    if exist "%DEST%\%PLUGIN%.dll" del /f /q "%DEST%\%PLUGIN%.dll"
    if exist "%DEST%\%PLUGIN%.pdb" del /f /q "%DEST%\%PLUGIN%.pdb"
)

REM Verify (robocopy's exit code alone can't be trusted).
if "%LAYOUT%"=="flat" (
    if not exist "%DEST%\%PLUGIN%.dll" goto :copy_fail
) else (
    if not exist "%DEST%\bin\64bit\%PLUGIN%.dll" goto :copy_fail
)
if not exist "%DEST%\data\locale\ja-JP.ini" goto :copy_fail

echo.
echo ============================================================
echo   INSTALL COMPLETE
echo ============================================================
if "%HAS_LEGACY%"=="1" (
    echo   Removed      : old copies in "%OBS_DIR%"
) else (
    echo   Removed      : nothing ^(no old copies found^)
)
echo   Installed to : %DEST%
echo.
echo   Start OBS and add "Font Effect Tools" from Add Source.
echo ============================================================
if "%NOPAUSE%"=="0" (
    powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; $m = [regex]::Unescape('Font Effect Tools \u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u304c\u5b8c\u4e86\u3057\u307e\u3057\u305f\u3002\n\n\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u5148:\n') + $env:DEST + [regex]::Unescape('\n\nOBS \u3092\u8d77\u52d5\u3057\u3066\u3001[\u30bd\u30fc\u30b9\u306e\u8ffd\u52a0] \u2192 [Font Effect Tools] \u304b\u3089\u4f7f\u3048\u307e\u3059\u3002'); [void][System.Windows.MessageBox]::Show($m, 'Font Effect Tools', 'OK', 'Information')" >nul 2>&1
)
goto :ok

:copy_fail
echo [error] Copy into "%DEST%" failed.
goto :fail

:ok
if "%NOPAUSE%"=="0" pause
endlocal
exit /b 0

:fail
echo.
echo ============================================================
echo   *** INSTALL FAILED ***  See the messages above.
echo ============================================================
if "%NOPAUSE%"=="0" (
    powershell -NoProfile -Command "Add-Type -AssemblyName PresentationFramework; $m = [regex]::Unescape('Font Effect Tools \u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002\n\n\u30a6\u30a3\u30f3\u30c9\u30a6\u306b\u8868\u793a\u3055\u308c\u305f\u30e1\u30c3\u30bb\u30fc\u30b8\u3092\u78ba\u8a8d\u3057\u3066\u304f\u3060\u3055\u3044\u3002'); [void][System.Windows.MessageBox]::Show($m, 'Font Effect Tools', 'OK', 'Error')" >nul 2>&1
)
if "%NOPAUSE%"=="0" pause
endlocal
exit /b 1
