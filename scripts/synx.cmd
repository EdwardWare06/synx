@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0vcvars-win.cmd" || exit /b 1
if exist "%ROOT%\bin\synx.exe" (
  "%ROOT%\bin\synx.exe" %*
) else (
  "%ROOT%\build-win\compiler\synx.exe" %*
)
exit /b %errorlevel%
