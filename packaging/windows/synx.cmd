@echo off
rem The installed Synx compiler uses the built-in linker by default.
rem --linker=system requires a developer-provided linker environment.
setlocal
set "SYNX_HOME=%~dp0.."
"%SYNX_HOME%\libexec\synx.exe" %*
exit /b %errorlevel%
