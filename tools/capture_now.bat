@echo off
rem Run while tools\capture_run.bat's game shows the problem.
cd /d "%~dp0.."
type nul > out\cap\user.grab.req
type nul > out\cap\user.trigger
echo Capturing the next 30 frames and a screenshot. Keep the view still for a second.
