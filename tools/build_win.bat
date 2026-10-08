@echo off
rem Windows build: clang-cl (LLVM) inside a Visual Studio developer environment.
rem Usage:  tools\build_win.bat [configure|build|all] [ninja args]   (default: all)
rem Builds the 1.01 update's lift (recompiled_101/) into build. DOD3_EBOOT=100
rem builds the disc version (recompiled/, unmaintained) into build_100.
rem
rem Needs Visual Studio 2022 or newer (the Build Tools are enough), LLVM with
rem clang-cl (on PATH, or in C:\Program Files\LLVM), CMake and Ninja, and
rem vcpkg with zlib:x64-windows (VCPKG_ROOT, or C:\vcpkg).
setlocal
set ACTION=%1
if "%ACTION%"=="" set ACTION=all
set BDIR=build
set EBOOT_ARG=-DDOD3_EBOOT=101
if "%DOD3_EBOOT%"=="100" set BDIR=build_100
if "%DOD3_EBOOT%"=="100" set EBOOT_ARG=-DDOD3_EBOOT=100
if not defined VCPKG_ROOT set VCPKG_ROOT=C:\vcpkg

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VCVARS=
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" echo build_win: Visual Studio was not found (vcvars64.bat) & exit /b 1
call "%VCVARS%" >nul
for %%c in (clang-cl.exe) do set CLANGCL=%%~$PATH:c
if not defined CLANGCL set CLANGCL=%ProgramFiles%\LLVM\bin\clang-cl.exe
if not exist "%CLANGCL%" echo build_win: clang-cl was not found (install LLVM) & exit /b 1
for %%c in (cmake.exe) do set CMAKE=%%~$PATH:c
if not defined CMAKE set CMAKE=%ProgramFiles%\CMake\bin\cmake.exe
if not exist "%CMAKE%" echo build_win: cmake was not found & exit /b 1

cd /d "%~dp0.."
if "%ACTION%"=="build" goto build
"%CMAKE%" -S . -B %BDIR% -G Ninja %EBOOT_ARG% ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="%CLANGCL:\=/%" ^
  -DCMAKE_CXX_COMPILER="%CLANGCL:\=/%" ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT:\=/%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
if %errorlevel% neq 0 exit /b 1
if "%ACTION%"=="configure" exit /b 0
:build
"%CMAKE%" --build %BDIR% -- %2 %3 %4
exit /b %errorlevel%
