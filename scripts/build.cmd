@echo off
rem Compila o launcher em out\: Visual Studio 2022 (ou Build Tools) com "Desenvolvimento para desktop com C++" e o
rem componente "C++ Clang". O CMake baixa SDL3, toml++ e Dear ImGui na primeira vez.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio 2022 com C++ nao encontrado.& exit /b 1)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "CM=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CM%\CMake\bin;%CM%\Ninja;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~dp0.."
if not exist out\build.ninja cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
cmake --build out
