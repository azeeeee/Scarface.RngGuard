@echo off
rem Build Scarface.RngGuard.asi (32-bit) for scarface.exe 1.0.0.2 / 1.0.0.1 layout
setlocal
cd /d "%~dp0"

rem Locate Visual Studio via vswhere, fall back to a known path.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR if exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars32.bat"
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"

if not exist "%VCVARS%" (
  echo vcvars32.bat not found.
  echo Install the "Desktop development with C++" workload with the x86 toolset,
  echo or edit VCVARS in this file to point at your vcvars32.bat.
  exit /b 1
)
call "%VCVARS%" >nul

if not exist dist mkdir dist
if not exist build mkdir build

cl /nologo /LD /MT /O2 /GS- /W3 src\RngGuard.cpp ^
   /Fe:"dist\Scarface.RngGuard.asi" ^
   /Fo:"build\\" ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X86 /NOLOGO

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo.
echo Built: dist\Scarface.RngGuard.asi
