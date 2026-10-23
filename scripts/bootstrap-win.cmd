@echo off
setlocal
rem Usage: bootstrap-win.cmd [stage0.exe] [output-dir] [--release] [--no-regalloc]
set "SYNX_BOOT_ROOT=%~dp0.."
set "SYNX_BOOT_STAGE0=%~1"
if "%SYNX_BOOT_STAGE0%"=="" set "SYNX_BOOT_STAGE0=%SYNX_BOOT_ROOT%\build-win\compiler\synx.exe"
set "SYNX_BOOT_OUTPUT=%~2"
if "%SYNX_BOOT_OUTPUT%"=="" set "SYNX_BOOT_OUTPUT=%SYNX_BOOT_ROOT%\build-win-bootstrap"
call "%~dp0vcvars-win.cmd" || exit /b 1
if not exist "%SYNX_BOOT_OUTPUT%" mkdir "%SYNX_BOOT_OUTPUT%"
if errorlevel 1 exit /b 1
set "SYNX_STD_PATH=%SYNX_BOOT_ROOT%\std"
if not defined SYNX_RUNTIME_LIB set "SYNX_RUNTIME_LIB=%SYNX_BOOT_ROOT%\build-win\runtime\synx_rt.lib"
rem Stages 2 and 3 link with the built-in linker, which needs the shared runtime.
if not defined SYNX_RUNTIME_SHARED set "SYNX_RUNTIME_SHARED=%SYNX_BOOT_ROOT%\build-win\runtime\synx_rt.dll"
"%SYNX_BOOT_STAGE0%" build "%SYNX_BOOT_ROOT%\selfhost\bootstrap.snx" -o "%SYNX_BOOT_OUTPUT%\bootstrap-runner.exe"
if errorlevel 1 exit /b 1
"%SYNX_BOOT_OUTPUT%\bootstrap-runner.exe" "%SYNX_BOOT_STAGE0%" "%SYNX_BOOT_ROOT%" "%SYNX_BOOT_OUTPUT%" "%~3" "%~4"
exit /b %errorlevel%
