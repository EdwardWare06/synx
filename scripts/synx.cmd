@echo off
rem Runs the Synx compiler from this source tree: bin\synx.exe if present,
rem else the stage 2 compiler that scripts\build-win.cmd builds.
setlocal
set "ROOT=%~dp0.."
if not defined SYNX_STD_PATH set "SYNX_STD_PATH=%ROOT%\std"
if exist "%ROOT%\bin\synx.exe" (
  "%ROOT%\bin\synx.exe" %*
) else (
  "%ROOT%\build-win\selfhost-stages\synx-stage2.exe" %*
)
exit /b %errorlevel%
