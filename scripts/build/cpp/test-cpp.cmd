@echo off
rem Run the reduced official C++ CTest suite. Pass-through arg: -Config <Debug|Release>
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-cpp.ps1" %*
exit /b %ERRORLEVEL%