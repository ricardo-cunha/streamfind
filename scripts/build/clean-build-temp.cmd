@echo off
setlocal EnableExtensions

rem Remove generated C++, Rust, and repository temporary output.
rem Run from any working directory; the repository root is derived from this file.
rem
rem Default behaviour: remove build/test artifacts and disposable scratch, but
rem PRESERVE tmp\scripts (development-support wrappers) and tmp\logs (build,
rem test, server, and session logs) so the dev loop and diagnostics survive a
rem routine clean.
rem
rem   clean-build-temp.cmd          -> build/test artifacts only
rem   clean-build-temp.cmd --all    -> also wipe tmp\scripts and tmp\logs
rem
rem Committed convenience scripts live under scripts\ and are never touched.
rem The tracked scripts\dev directory is not deleted by this command.

set "ROOT=%~dp0..\.."

call :remove_dir "%ROOT%\build" "root CMake/build output"
call :remove_dir "%ROOT%\core\build" "standalone C++ core build output"
call :remove_dir "%ROOT%\core\vendor\openbabel\build" "vendored OpenBabel build output"
call :remove_dir "%ROOT%\rust\target" "Rust target output"
call :remove_dir "%ROOT%\integrations\cf-streamfind\build" "Cogniflow integration build output"
call :remove_dir "%ROOT%\dist" "distribution output"
call :remove_dir "%ROOT%\site" "documentation site output"
call :remove_dir "%ROOT%\_skbuild" "scikit-build output"
call :remove_dir "%ROOT%\log" "legacy repository logs (folded into tmp\logs)"
call :remove_dir "%ROOT%\cache" "legacy repository cache"

rem Build/test artifacts and disposable scratch under tmp\ (always removed).
call :remove_dir "%ROOT%\tmp\build" "temporary build trees (CMake/Cargo)"
call :remove_dir "%ROOT%\tmp\projects" "temporary test project files (DuckDB fixtures)"
call :remove_dir "%ROOT%\tmp\scratch" "temporary scratch files"
call :remove_dir "%ROOT%\tmp\release-output" "temporary release packages"

rem Development-support scripts and logs are kept by default.
rem --all also wipes them (call once a supported feature is implemented).
if /i "%~1"=="--all" (
    call :remove_dir "%ROOT%\tmp\scripts" "legacy development-support scripts"
    call :remove_dir "%ROOT%\tmp\logs" "build/test/session logs"
    call :remove_dir "%ROOT%\tmp" "repository temporary folder (remaining)"
)

for /d %%D in ("%ROOT%\cmake-build-*") do (
    if exist "%%~fD\" call :remove_dir "%%~fD" "CMake build directory"
)

rem Python environments and their caches are intentionally left untouched.

if errorlevel 1 (
    echo Cleanup completed with errors. 1>&2
    exit /b 1
)

echo Generated build and temporary output removed.
exit /b 0

:remove_dir
if not exist "%~1\" exit /b 0
echo Removing %~2: "%~1"
rd /s /q "%~1"
exit /b %errorlevel%