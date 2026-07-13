@echo off
rem Double-click to launch Easy Setup. Builds it first if the exe is missing.
if not exist "%~dp0EasySetup.exe" (
    echo EasySetup.exe not found - building it first...
    call "%~dp0Build.bat"
)
start "" "%~dp0EasySetup.exe"
