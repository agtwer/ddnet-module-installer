@echo off
rem One-click build for ddnet-module-installer (uses the CMake bundled with Visual Studio).
rem ASCII-only on purpose: a .bat that contains non-ASCII bytes (no BOM) breaks cmd's parser.
setlocal

set "CMAKE="
for %%P in (
  "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do (
  if exist %%P set "CMAKE=%%~P"
)
if "%CMAKE%"=="" (
  where cmake >nul 2>nul && set "CMAKE=cmake"
)
if "%CMAKE%"=="" (
  echo [x] cmake not found. Run this from the Visual Studio Developer PowerShell, or add cmake to PATH.
  exit /b 1
)

echo [i] using: %CMAKE%
"%CMAKE%" -S . -B build -A x64 || exit /b 1
"%CMAKE%" --build build --config Release || exit /b 1

echo.
echo [ok] output: build\Release\ddnet-module-installer.exe
endlocal
