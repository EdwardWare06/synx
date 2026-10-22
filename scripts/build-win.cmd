@echo off
rem Build and test the Synx compiler on Windows with MSVC.
rem Usage: scripts\build-win.cmd [Debug|Release]
rem Needs CMake and Visual Studio or Build Tools with the C++ workload.

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug
set ROOT=%~dp0..
set BUILD=%ROOT%\build-win

call "%~dp0vcvars-win.cmd" || exit /b 1

cmake -S "%ROOT%" -B "%BUILD%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1
"%BUILD%\compiler\synx.exe" version
