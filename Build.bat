@echo off
setlocal
rem Locate Visual Studio and set up the x64 build environment
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo Could not find Visual Studio. Make sure it's installed.
    pause & exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo Failed to init VS environment & pause & exit /b 1 )

echo Compiling icon resource ...
rc /nologo /fo "%~dp0app.res" "%~dp0app.rc"
if errorlevel 1 ( echo. & echo RESOURCE BUILD FAILED & pause & exit /b 1 )

echo Compiling EasySetup.cpp ...
cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE "%~dp0EasySetup.cpp" "%~dp0app.res" ^
   /Fe:"%~dp0EasySetup.exe" /Fo:"%~dp0build\\" ^
   /link /SUBSYSTEM:WINDOWS

if errorlevel 1 ( echo. & echo BUILD FAILED & pause & exit /b 1 )
echo. & echo Build OK -^> EasySetup.exe
