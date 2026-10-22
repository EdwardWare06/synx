@echo off
rem Loads the x64 MSVC environment into the calling script.
rem Usage: call "%~dp0vcvars-win.cmd" || exit /b 1
rem Does nothing when cl.exe is already on PATH, as in an x64 Native Tools
rem command prompt. Otherwise vswhere finds the newest Visual Studio or Build
rem Tools instance that has the x64 C++ tools.
where cl.exe >nul 2>&1
if not errorlevel 1 exit /b 0
set "SYNX_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "SYNX_VSROOT="
if not exist "%SYNX_VSWHERE%" goto missing
for /f "usebackq delims=" %%i in (`"%SYNX_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "SYNX_VSROOT=%%i"
if not defined SYNX_VSROOT goto missing
if not exist "%SYNX_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto missing
call "%SYNX_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "SYNX_VSWHERE="
set "SYNX_VSROOT="
where cl.exe >nul 2>&1
if not errorlevel 1 exit /b 0
:missing
set "SYNX_VSWHERE="
set "SYNX_VSROOT="
echo Visual Studio Build Tools with the C++ workload were not found.
echo Install them, or run this script from an x64 Native Tools command prompt.
exit /b 1
