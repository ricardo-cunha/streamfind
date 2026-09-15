@echo off
rem Build and package the alternative Rust backend against a C++ catalogue.
rem Required: -CppCatalogue <path-to-cpp-catalogue.duckdb>
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release-rust.ps1" %*
exit /b %ERRORLEVEL%
