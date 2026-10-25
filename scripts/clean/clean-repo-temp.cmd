@echo off
setlocal EnableExtensions

rem Remove disposable repository output and Python caches.
rem Run from any working directory; the repository root is derived from this file.
rem The command intentionally removes all contents of tmp, including logs,
rem packaged builds, project fixtures, build trees, and release archives.
rem It never removes tracked source, scripts, the repository .venv, node_modules,
rem or vendored third-party sources. Python caches are scanned only in the
rem repository-owned source/script trees to avoid traversing dependency trees.

set "ROOT=%~dp0..\.."
set "FAILED=0"

rem Generated output outside tmp.
call :remove_dir "%ROOT%\build" "root CMake/build output"
call :remove_dir "%ROOT%\cpp\build" "C++ in-tree build output"
call :remove_dir "%ROOT%\core\build" "standalone C++ core build output"
call :remove_dir "%ROOT%\core\vendor\openbabel\build" "vendored OpenBabel build output"
call :remove_dir "%ROOT%\dist" "distribution output"
call :remove_dir "%ROOT%\cpp\dist" "C++ distribution output"
call :remove_dir "%ROOT%\frontend\dist" "frontend production output"
call :remove_dir "%ROOT%\test-results" "repository test results"
call :remove_dir "%ROOT%\cpp\test-results" "C++ test results"
call :remove_dir "%ROOT%\coverage" "test coverage output"
call :remove_dir "%ROOT%\cpp\coverage" "C++ coverage output"
call :remove_dir "%ROOT%\frontend\coverage" "frontend coverage output"
call :remove_dir "%ROOT%\site" "documentation site output"
call :remove_dir "%ROOT%\_skbuild" "scikit-build output"
call :remove_dir "%ROOT%\log" "legacy repository logs"
call :remove_dir "%ROOT%\cache" "legacy repository cache"

for /d %%D in ("%ROOT%\cmake-build-*") do (
    if exist "%%~fD\" call :remove_dir "%%~fD" "CMake build directory"
)

rem tmp is entirely disposable. Recreate only the anchor directory so the next
rem build/test script can create its own managed subdirectories.
call :remove_dir "%ROOT%\tmp" "all repository temporary output"
if exist "%ROOT%\tmp" goto :tmp_created
mkdir "%ROOT%\tmp" 2>nul
if errorlevel 1 set "FAILED=1"

:tmp_created
echo Removing Python cache directories and bytecode...
call :remove_python_caches "%ROOT%\scripts"
call :remove_python_caches "%ROOT%\cpp\core"
call :remove_python_caches "%ROOT%\cpp\plugins"
call :remove_python_caches "%ROOT%\cpp\service"
call :remove_python_caches "%ROOT%\cpp\tests"
call :remove_python_caches "%ROOT%\bindings"
call :remove_python_caches "%ROOT%\semantic"
call :remove_python_caches "%ROOT%\tests"
call :remove_file "%ROOT%\.coverage" "Python coverage data"

if "%FAILED%"=="1" (
    echo Repository cleanup completed with errors. 1>&2
    exit /b 1
)

echo Repository temporary output and Python caches removed.
exit /b 0

:remove_dir
if not exist "%~1\" exit /b 0
echo Removing %~2: "%~1"
rd /s /q "%~1"
if errorlevel 1 set "FAILED=1"
exit /b 0

:remove_file
if not exist "%~1" exit /b 0
echo Removing %~2: "%~1"
del /f /q "%~1"
if errorlevel 1 set "FAILED=1"
exit /b 0

:remove_python_caches
if not exist "%~1\" exit /b 0
for /d /r "%~1" %%D in (__pycache__ .pytest_cache .mypy_cache .ruff_cache .hypothesis) do (
    if exist "%%~fD\" call :remove_dir "%%~fD" "Python cache: %%~fD"
)
for /r "%~1" %%F in (*.pyc *.pyo) do (
    if exist "%%~fF" (
        del /f /q "%%~fF" >nul 2>&1
        if errorlevel 1 set "FAILED=1"
    )
)
exit /b 0
