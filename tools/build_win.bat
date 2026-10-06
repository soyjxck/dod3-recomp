@echo off
rem Windows build: clang-cl (LLVM) inside a VS 2022 developer environment.
rem Usage:  tools\build_win.bat [configure|build|all]   (default: all)
setlocal
set ACTION=%1
if "%ACTION%"=="" set ACTION=all
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if "%ACTION%"=="build" goto build
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
if %errorlevel% neq 0 exit /b 1
if "%ACTION%"=="configure" exit /b 0
:build
"C:\Program Files\CMake\bin\cmake.exe" --build build -- %2 %3 %4
exit /b %errorlevel%
