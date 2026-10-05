@echo off
rem 编译执行器（32 位；执行器本身是宿主程序，位数不敏感，这里跟 oracle 保持一致用 x86）
setlocal
set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" (
  echo [build] vcvarsall.bat not found; edit VCVARS in this script.
  exit /b 1
)
call "%VCVARS%" x64 >nul
rem /utf-8 必须加：源文件是 UTF-8，而 MSVC 默认按本地代码页读，中文注释会被错解、
rem 把后面的代码吞进注释里（表现为莫名其妙的"语法错误"，见 L10N 排查记录）。
cl /nologo /W3 /O2 /utf-8 /Fe:emu.exe emu.c
