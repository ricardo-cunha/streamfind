@echo off
rem Build and package the authoritative C++ backend release.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release-cpp.ps1" %*
exit /b %ERRORLEVEL%
