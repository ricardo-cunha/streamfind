@echo off
rem Run the reduced official Rust workspace test suite. Pass-through args: -Package <name> -Release
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-rust.ps1" %*
exit /b %ERRORLEVEL%