@echo off
chcp 65001 >nul
setlocal
cd /d %~dp0

rem Find the local Visual Studio C++ toolchain; if not found, edit the VCVARS path below.
set "VS="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
)
if "%VS%"=="" (
  echo [ERROR] Visual Studio C++ toolchain not found.
  exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed.
  exit /b 1
)

cl /nologo /EHsc /std:c++17 /W4 /utf-8 sms.cpp /Fe:sms.exe /link ws2_32.lib
if errorlevel 1 (
  echo [ERROR] build failed.
  exit /b 1
)
del /q sms.obj 2>nul
echo.
echo [OK] sms.exe built.
echo Usage:
echo   sms.exe            console menu + web server on port 4399
echo   sms.exe --server   web server only
exit /b 0
