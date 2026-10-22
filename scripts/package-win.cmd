@echo off
rem Builds an installable Synx package on Windows.
rem Usage: scripts\package-win.cmd [synx|cpp]
rem   synx (default) installs the self-hosted compiler (stage 2 of the bootstrap).
rem   cpp installs the C++ compiler instead.
rem Output: dist\packages\synx-<version>-windows-x86_64[-cpp].zip
setlocal
call "%~dp0vcvars-win.cmd" || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package-win.ps1" %*
exit /b %errorlevel%
