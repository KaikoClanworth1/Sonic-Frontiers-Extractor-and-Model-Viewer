@echo off
REM Configure + build with MSVC 2019 BuildTools (bundled CMake + Ninja).
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools"
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "TARGET=%1"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%CMAKE%" -S "%ROOT%." -B "%BUILD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

if "%TARGET%"=="" (
  "%CMAKE%" --build "%BUILD%"
) else (
  "%CMAKE%" --build "%BUILD%" --target %TARGET%
)
