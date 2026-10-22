@echo off
rem Installs this Synx package for the current user. Arguments pass through
rem to install.ps1: -Destination <dir>, -NoPath, -Uninstall.
rem One line, because -Uninstall can delete this file while it runs.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %* && exit /b 0 || exit /b 1
