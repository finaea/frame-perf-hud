@echo off
rem Release G: the PC half of a release (see docs\RELEASING.md). A clean Release build of the exe, copied to
rem dist\FramePerfHud-<version>.exe, and its line for SHA256SUMS printed in sha256sum's format. Publishes nothing.
rem   pc\package.cmd
rem The version is CMakeLists.txt's project(VERSION). The build goes to out\pc\build-release (FPH_PC_BUILD_DIR
rem overrides), so the everyday out\pc\build and a copy running from it are left alone.
setlocal
set "REPO=%~dp0.."
for %%I in ("%REPO%") do set "REPO=%%~fI"
set "VERSION="
for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "$m = Select-String -Path '%REPO%\CMakeLists.txt' -Pattern 'project\(frame-perf-hud VERSION ([0-9.]+)'; if ($m) { $m.Matches[0].Groups[1].Value }"`) do set "VERSION=%%V"
if not defined VERSION (
  echo Couldn't read the version from CMakeLists.txt
  exit /b 1
)
if not defined FPH_PC_BUILD_DIR set "FPH_PC_BUILD_DIR=%REPO%\out\pc\build-release"
call "%~dp0build.cmd" release || exit /b 1
if not exist "%REPO%\dist" mkdir "%REPO%\dist"
set "NAME=FramePerfHud-%VERSION%.exe"
set "OUT=%REPO%\dist\%NAME%"
copy /y "%FPH_PC_BUILD_DIR%\pc\FramePerfHud.exe" "%OUT%" >nul || exit /b 1
set "HASH="
for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%OUT%').Hash.ToLower()"`) do set "HASH=%%H"
if not defined HASH (
  echo Couldn't hash %OUT%
  exit /b 1
)
echo.
echo Wrote %OUT%
echo Append this line to the release's SHA256SUMS (next to the headset tarball's):
echo %HASH%  %NAME%
exit /b 0
