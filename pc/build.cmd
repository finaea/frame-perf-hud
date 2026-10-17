@echo off
rem Builds the Windows app (ticket 07): out\pc\build\pc\FramePerfHud.exe, plus the pc-* tests.
rem   pc\build.cmd            configure (first run: vcpkg builds cairo + freetype, ~10 min) and build
rem   pc\build.cmd test       ... then run the PC tests
rem   pc\build.cmd release    a clean Release build of the exe only, no tests (pc\package.cmd uses it)
rem FPH_PC_BUILD_DIR picks another build folder (default out\pc\build), e.g. to leave a running copy alone.
rem Needs Visual Studio 2022 (C++ workload; its CMake + Ninja are used) and a vcpkg checkout: FPH_VCPKG, else
rem ..\vcpkg next to the repo. Nothing system-wide is written: packages go to out\pc\vcpkg_installed, vcpkg's
rem downloads stay in its checkout, and its binary cache sits next to it (vcpkg-cache).
rem FPH_VCPKG rather than VCPKG_ROOT: vcvars64 sets VCPKG_ROOT to VS's bundled vcpkg, which keeps its files in
rem %LOCALAPPDATA%\vcpkg.
setlocal
set "REPO=%~dp0.."
for %%I in ("%REPO%") do set "REPO=%%~fI"
if not defined FPH_VCPKG set "FPH_VCPKG=%REPO%\..\vcpkg"
for %%I in ("%FPH_VCPKG%") do set "FPH_VCPKG=%%~fI"
if not exist "%FPH_VCPKG%\scripts\buildsystems\vcpkg.cmake" (
  echo vcpkg not found at %FPH_VCPKG%: set FPH_VCPKG, or git clone https://github.com/microsoft/vcpkg there and run bootstrap-vcpkg.bat
  exit /b 1
)
if not defined VCPKG_DEFAULT_BINARY_CACHE set "VCPKG_DEFAULT_BINARY_CACHE=%FPH_VCPKG%\..\vcpkg-cache"
if not exist "%VCPKG_DEFAULT_BINARY_CACHE%" mkdir "%VCPKG_DEFAULT_BINARY_CACHE%"

rem (vswhere's path holds "(x86)", which would end a parenthesised block: keep it outside one)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined VSCMD_VER goto have_vs
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%V"
if not defined VSDIR (
  echo Visual Studio 2022 with the C++ workload wasn't found
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs
set "VCPKG_ROOT=%FPH_VCPKG%"
where ninja >nul 2>nul || set "PATH=%PATH%;%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

set "BUILD=%REPO%\out\pc\build"
if defined FPH_PC_BUILD_DIR set "BUILD=%FPH_PC_BUILD_DIR%"
set "TESTING=ON"
set "TARGET=fph-pc-all"
set "CLEAN="
if /i "%~1"=="release" (
  set "TESTING=OFF"
  set "TARGET=frame-perf-hud-pc"
  set "CLEAN=--clean-first"
)
set "INSTALLED=%REPO%\out\pc\vcpkg_installed"
cmake -S "%REPO%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=%FPH_VCPKG%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows ^
  "-DVCPKG_MANIFEST_DIR=%REPO%\pc" "-DVCPKG_INSTALLED_DIR=%INSTALLED%" ^
  "-DPKG_CONFIG_EXECUTABLE=%INSTALLED%\x64-windows\tools\pkgconf\pkgconf.exe" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DFPH_BUILD_HEADSET=OFF -DFPH_BUILD_PC=ON -DBUILD_TESTING=%TESTING% || exit /b 1
cmake --build "%BUILD%" --target %TARGET% %CLEAN% || exit /b 1
echo Built %BUILD%\pc\FramePerfHud.exe
if /i "%~1"=="test" (
  ctest --test-dir "%BUILD%" -R "^pc-" --output-on-failure || exit /b 1
)
exit /b 0
