@echo off
rem Builds an installable Synx package on Windows.
rem Usage: scripts\package-win.cmd
rem Installs the compiler built by the seed release (stage 2) and checks it.
rem Output: dist\packages\synx-<version>-windows-x86_64.zip
setlocal
call "%~dp0vcvars-win.cmd" || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package-win.ps1" %*
exit /b %errorlevel%
