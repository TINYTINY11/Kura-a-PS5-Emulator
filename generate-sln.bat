@echo off
rem Generates Kura.sln for Visual Studio 2026 using its bundled CMake.
rem If your Visual Studio is installed elsewhere, edit the CMAKE variable below.
setlocal
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

echo Generating Kura.sln with: %CMAKE%
"%CMAKE%" -S . -B . -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
  echo.
  echo FAILED. Check that Visual Studio 2026 is installed, or edit the CMAKE= path in this file.
  exit /b 1
)
echo.
echo Done — open Kura.sln in Visual Studio 2026.
