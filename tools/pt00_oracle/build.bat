@echo off
rem 编译 32 位 oracle（需要 VS 的 x86 工具链）。
rem 用法：按需改下面的 vcvarsall 路径，然后运行 tools\pt00_oracle\build.bat
setlocal
set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" (
  echo [build] vcvarsall.bat not found; edit VCVARS in this script.
  exit /b 1
)
call "%VCVARS%" x86 >nul
cl /nologo /W3 /O2 /Fe:oracle.exe oracle.c
