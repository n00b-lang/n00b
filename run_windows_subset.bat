@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "BIN_DIR=%ROOT%build_cross_windows_x86_64"
set "LOG_DIR=%ROOT%windows_debug_logs"
set "SUMMARY=%LOG_DIR%\summary.txt"
set "OVERALL=0"
set "DBG_CMD="
if not defined N00B_WIN_THREAD_DEBUG set "N00B_WIN_THREAD_DEBUG=1"

if not exist "%BIN_DIR%\" (
    echo [ERROR] Missing directory: "%BIN_DIR%"
    exit /b 2
)

if not exist "%LOG_DIR%\" mkdir "%LOG_DIR%"

call :detect_debugger

(
    echo n00b windows subset run
    echo root: %ROOT%
    echo bin_dir: %BIN_DIR%
    if defined DBG_CMD (
        echo debugger: %DBG_CMD%
    ) else (
        echo debugger: not found ^(cdb.exe not available in PATH^)
    )
    echo N00B_WIN_THREAD_DEBUG: %N00B_WIN_THREAD_DEBUG%
    echo.
) > "%SUMMARY%"

if "%ROOT:~0,2%"=="\\" (
    echo [WARN] Running from UNC path: "%ROOT%"
    echo [WARN] Copy this folder to a local NTFS path such as C:\tmp\n00b before running tests.
    (
        echo warning: running from UNC path. this can cause loader/runtime faults.
        echo warning: copy to local NTFS path, e.g. C:\tmp\n00b.
        echo.
    ) >> "%SUMMARY%"
)

call :run_test testie
call :run_test test_list
call :run_test test_hash

echo.
echo Overall exit code: %OVERALL%
echo Logs written to: "%LOG_DIR%"
>> "%SUMMARY%" echo Overall exit code: %OVERALL%
exit /b %OVERALL%

:detect_debugger
for %%I in (cdbX64.exe cdbX64 cdbx64.exe cdbx64 cdb.exe cdb) do (
    if not defined DBG_CMD (
        where %%I >nul 2>&1
        if not errorlevel 1 set "DBG_CMD=%%I"
    )
)

if not defined DBG_CMD (
    if exist "%ProgramFiles%\Windows Kits\10\Debuggers\x64\cdb.exe" (
        set "DBG_CMD=%ProgramFiles%\Windows Kits\10\Debuggers\x64\cdb.exe"
    )
)

if not defined DBG_CMD (
    if exist "%LocalAppData%\Microsoft\WindowsApps\cdbX64.exe" (
        set "DBG_CMD=%LocalAppData%\Microsoft\WindowsApps\cdbX64.exe"
    )
)
exit /b 0

:run_test
set "NAME=%~1"
set "EXE=%BIN_DIR%\%NAME%.exe"
set "RUN_LOG=%LOG_DIR%\%NAME%_stdout.txt"
set "DBG_LOG=%LOG_DIR%\%NAME%_cdb.txt"
set "EVT_LOG=%LOG_DIR%\%NAME%_event1000.txt"

if not exist "%EXE%" (
    echo [ERROR] Missing executable: "%EXE%"
    if "%OVERALL%"=="0" set "OVERALL=3"
    (
        echo [%NAME%]
        echo exe: %EXE%
        echo exit_code: missing_executable
        echo.
    ) >> "%SUMMARY%"
    echo.
    exit /b 0
)

echo ===== Running %NAME%.exe =====
echo ===== Running %NAME%.exe ===== > "%RUN_LOG%"
"%EXE%" >> "%RUN_LOG%" 2>&1
set "CODE=!ERRORLEVEL!"
type "%RUN_LOG%"
echo %NAME%.exe exit code: !CODE!
echo.

(
    echo [%NAME%]
    echo exe: %EXE%
    echo exit_code: !CODE!
    echo run_log: %RUN_LOG%
) >> "%SUMMARY%"

if not "!CODE!"=="0" (
    if "%OVERALL%"=="0" set "OVERALL=!CODE!"
    call :collect_debug "%EXE%" "%NAME%" "%DBG_LOG%"
    call :collect_event "%NAME%" "%EVT_LOG%"
    (
        echo debug_log: %DBG_LOG%
        echo event_log: %EVT_LOG%
    ) >> "%SUMMARY%"
)

>> "%SUMMARY%" echo.
exit /b 0

:collect_debug
set "EXE=%~1"
set "NAME=%~2"
set "DBG_LOG=%~3"
set "SYM_DIR=%~dp1"

if not defined DBG_CMD (
    (
        echo cdb.exe not found.
        echo install Debugging Tools for Windows and ensure cdb.exe is in PATH.
    ) > "%DBG_LOG%"
    echo [WARN] cdb.exe not found; skipped debugger capture for %NAME%.
    exit /b 0
)

echo [INFO] Capturing debugger output for %NAME%...
setlocal DisableDelayedExpansion
set "CDB_CMDS=%TEMP%\n00b_cdb_%RANDOM%_%RANDOM%.txt"
(
    echo .symfix
    echo .sympath+ %SYM_DIR%
    echo .sympath
    echo .symopt+ 0x40
    echo .reload /f
    echo sxe av
    echo g
    echo .exr -1
    echo .ecxr
    echo r
    echo ln @rip
    echo u @rip-20 L40
    echo db @rip-20 L40
    echo !analyze -v
    echo kb
    echo lm
    echo q
) > "%CDB_CMDS%"
"%DBG_CMD%" -cf "%CDB_CMDS%" "%EXE%" > "%DBG_LOG%" 2>&1
del /q "%CDB_CMDS%" >nul 2>&1
endlocal
echo [INFO] Wrote debugger log: "%DBG_LOG%"
exit /b 0

:collect_event
set "NAME=%~1"
set "EVT_LOG=%~2"

powershell -NoProfile -Command "$n='%NAME%.exe'; $e=Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 50 | Where-Object { $_.Message -like ('*' + $n + '*') } | Select-Object -First 1; if ($null -eq $e) { 'No matching Application Error (Event ID 1000) found.' | Out-File -FilePath '%EVT_LOG%' -Encoding utf8 } else { $e | Format-List TimeCreated, Id, ProviderName, LevelDisplayName, Message | Out-File -FilePath '%EVT_LOG%' -Encoding utf8 }" >nul 2>&1
exit /b 0
