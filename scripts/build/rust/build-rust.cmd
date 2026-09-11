@echo off
rem Build the Rust workspace (tmp/build/rust-target). Pass-through args:
rem -Clean -Tests -Package <name> -Release
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-rust.ps1" %*
exit /b %ERRORLEVEL%