@echo off
rem Builds the Slinky client and the injector. See build.ps1 for the options.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
echo.
pause
