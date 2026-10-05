@echo off
rem Build the PT00 x86-32 interpreter (host program; 32-bit picked to match the oracle).
rem ASCII ONLY on purpose: cmd parses .bat with the console code page (936 on this box),
rem and UTF-8 comments + LF-only line endings get mis-split there (a CP936 lead byte
rem swallows the newline and merges the next line into the current command).
rem Keep this file ASCII, or re-save it as UTF-8 WITH BOM.
setlocal
cd /d "%~dp0"

set "VCVARS="
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
)
if not defined VCVARS (
  echo [build] vcvarsall.bat not found; edit this script and set VCVARS manually.
  exit /b 1
)
call "%VCVARS%" x64 >nul
rem /utf-8 is required: the sources are UTF-8, MSVC otherwise reads them as CP936 and
rem mis-decodes Chinese comments, swallowing the code that follows them.
cl /nologo /W3 /O2 /utf-8 /Fe:emu.exe emu.c
