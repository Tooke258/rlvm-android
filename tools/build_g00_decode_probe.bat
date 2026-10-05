@echo off
rem Build the host-side G00 decode probe. It compiles the SAME vendored decoder the
rem APK uses (rlvm-release-0.14/vendor/xclannad/file.cc), so its verdict on
rem "does this image decode to anything?" is directly transferable.
rem ASCII ONLY on purpose (see tools/pt00_emu/build.bat for the reason).
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
call "%VCVARS%" x86 >nul

rem The vendored decoder is old C++ (unistd/dirent habits); WIN32 selects its own
rem Windows path. We disable the optional png/jpeg/zlib/mmap paths on purpose.
rem /utf-8 is REQUIRED: the vendored file.cc has UTF-8 Japanese comments; without it
rem MSVC reads them as CP936, a trailing byte swallows a newline / the "*/" and the
rem parse collapses into bogus "undeclared identifier" errors.
cl /nologo /W0 /O2 /EHsc /utf-8 /DWIN32 /DHAVE_LIBPNG=0 /DHAVE_LIBJPEG=0 /DHAVE_LIBZ=0 /DHAVE_MMAP=0 ^
   /I "..\rlvm-release-0.14\vendor" ^
   /Fe:g00_decode_probe.exe g00_decode_probe.cpp ^
   "..\rlvm-release-0.14\vendor\xclannad\file.cc" "..\rlvm-release-0.14\vendor\xclannad\endian.cpp"
