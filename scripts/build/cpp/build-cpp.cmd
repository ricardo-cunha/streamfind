@echo off
rem Build the primary C++ backend (tmp/build/core-default). Pass-through args:
rem -Clean -Tests -Target <name> -Config <Debug|Release>
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-cpp.ps1" %*
exit /b %ERRORLEVEL%