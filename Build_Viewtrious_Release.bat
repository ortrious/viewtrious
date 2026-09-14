@echo off
setlocal
title Build Viewtrious Release

cd /d "%~dp0"

echo.
echo ========================================
echo   Building Viewtrious Release
echo ========================================
echo.

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto :missing_vcvars

call "%VCVARS%"
if errorlevel 1 goto :vcvars_failed

echo.
echo Building out\release\Viewtrious.exe...
echo.

cmake --build out\release
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
if not "%BUILD_EXIT_CODE%"=="0" goto :build_failed

echo.
echo ========================================
echo   BUILD SUCCEEDED
echo ========================================
echo.
echo EXE:
echo %CD%\out\release\Viewtrious.exe
echo.
goto :done

:missing_vcvars
echo ERROR: Visual Studio Build Tools environment script was not found:
echo "%VCVARS%"
echo.
goto :failed

:vcvars_failed
echo.
echo ERROR: Failed to initialize the MSVC build environment.
echo.
goto :failed

:build_failed
echo.
echo ========================================
echo   BUILD FAILED (exit code %BUILD_EXIT_CODE%)
echo ========================================
echo.
goto :failed

:failed
pause
exit /b 1

:done
pause
exit /b 0
