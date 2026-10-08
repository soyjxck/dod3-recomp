@echo off
rem Play with an RSX frame capture armed. Get to the problem, then run
rem tools\capture_now.bat: it records the next 30 frames to
rem out\cap\user.rsxcap and saves a screenshot to out\cap\user.grab.ppm.
rem The game log goes to out\cap\user.log. Settings come from the build
rem folder's dod3.ini. Optional argument: the build folder (default build).
cd /d "%~dp0.."
set BUILD=build
if not "%~1"=="" set BUILD=%~1
if exist out\cap\user.trigger del out\cap\user.trigger
set PS3_TITLE=Drakengard 3
set PS3_VFS_ROOT=game/disc
set RSX_CAPTURE=out/cap/user.rsxcap
set RSX_CAPTURE_TRIGGER=out/cap/user.trigger
set RSX_CAPTURE_FRAMES=30
set PS3RECOMP_FRAME_GRAB=out/cap/user.grab
%BUILD%\dod3.exe elf\EBOOT.ELF > out\cap\user.log 2>&1
