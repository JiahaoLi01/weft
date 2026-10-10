@echo off
setlocal EnableExtensions
rem Weft gallery runner. Double-click for the menu, or pass the entry number:
rem   tools\gallery.bat              menu
rem   tools\gallery.bat 2            gallery window with profiler traces
rem   tools\gallery.bat 1 novsync    gallery window with vertical sync off
rem Each run writes to artifacts\local\gallery-runs\<time>-<mode>. The Release probe is built when it is missing;
rem entry 6 rebuilds it after source changes. With vertical sync off the frame rate is not capped at the display
rem refresh rate; F7 toggles it inside the gallery and Canvas windows.

pushd "%~dp0.." || exit /b 1
set "EXE=build\windows-msvc\bin\Release\weft_d3d11_probe.exe"
set "CODE=0"
set "OUT="
set "CHOICE=%~1"
set "VSYNC="
if /i "%~2"=="novsync" set "VSYNC=--no-vsync"
set "INTERACTIVE="
if "%CHOICE%"=="" set "INTERACTIVE=1"
set "STAMP=run"
for /f "delims=" %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%t"

if not "%CHOICE%"=="" goto dispatch
:menu
set "VSYNC_STATE=on "
if defined VSYNC set "VSYNC_STATE=off"
echo.
echo   Weft gallery - Release build, hardware GPU
echo.
echo   1  Gallery window              4K catalogue of 72 cases; Esc closes, F7 toggles VSync
echo   2  Gallery window + profiler   owner/render traces, analyzed after Esc
echo   3  Live benchmarks             anime drag, scatter drag, Canvas; about 30 s, windows pop up
echo   4  Gallery self-test           hidden 4K run with pixel, geometry and input checks
echo   5  Canvas window               particles and fireworks; Tab switches, Esc closes, F7 toggles VSync
echo   6  Build                       Release weft_d3d11_probe
echo   V  VSync %VSYNC_STATE%                   for entries 1, 2, 3 and 5; off lifts the frame rate above the refresh rate
echo.
set "CHOICE="
set /p "CHOICE=Choose 1-6, or V to toggle VSync: "
if /i "%CHOICE%"=="V" (
    if defined VSYNC (set "VSYNC=") else (set "VSYNC=--no-vsync")
    goto menu
)

:dispatch
set "TAG="
if defined VSYNC set "TAG=-novsync"
if "%CHOICE%"=="6" goto build
if not exist "%EXE%" (
    call :build_probe
    if errorlevel 1 goto finish
)
if "%CHOICE%"=="1" goto window
if "%CHOICE%"=="2" goto profile
if "%CHOICE%"=="3" goto live
if "%CHOICE%"=="4" goto selftest
if "%CHOICE%"=="5" goto canvas
echo Unknown entry "%CHOICE%".
set "CODE=1"
goto done

:build
call :build_probe
goto finish

:window
set "OUT=artifacts\local\gallery-runs\%STAMP%-window%TAG%"
"%EXE%" --gallery-window-hardware "%OUT%" %VSYNC%
goto finish

:profile
set "OUT=artifacts\local\gallery-runs\%STAMP%-profile%TAG%"
echo Recording is on; F8 or the sidebar Profiler entry toggles it. Traces are written when the window closes.
"%EXE%" --gallery-window-profile-hardware "%OUT%" %VSYNC%
if errorlevel 1 goto finish
call :python tools\analyze_gallery_trace.py "%OUT%"
goto finish

:live
set "OUT=artifacts\local\gallery-runs\%STAMP%-live%TAG%"
set "FAILED="
for %%s in (anime-drag scatter-drag webgl) do (
    echo Live benchmark %%s ...
    "%EXE%" --gallery-live-benchmark-hardware "%OUT%\%%s" %%s %VSYNC%
    if errorlevel 1 set "FAILED=1"
)
call :python tools\summarize_gallery_live.py "%OUT%\anime-drag" "%OUT%\scatter-drag" "%OUT%\webgl"
if defined FAILED cmd /c exit 1
goto finish

:selftest
set "OUT=artifacts\local\gallery-runs\%STAMP%-selftest"
"%EXE%" --gallery-window-test-hardware "%OUT%"
goto finish

:canvas
set "OUT=artifacts\local\gallery-runs\%STAMP%-canvas%TAG%"
"%EXE%" --gallery-canvas-window-hardware "%OUT%" %VSYNC%
goto finish

:finish
set "CODE=%errorlevel%"
echo.
if not "%CODE%"=="0" echo Failed, exit code %CODE%.
if defined OUT echo Output: %CD%\%OUT%

:done
popd
if defined INTERACTIVE pause
exit /b %CODE%

rem Configures when needed and builds the Release probe.
:build_probe
where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake is not on PATH. Build weft_d3d11_probe in Visual Studio or a developer prompt.
    exit /b 1
)
if not exist "build\windows-msvc\CMakeCache.txt" (
    cmake --preset windows-msvc
    if errorlevel 1 exit /b 1
)
echo Building Release weft_d3d11_probe ...
cmake --build --preset windows-release --target weft_d3d11_probe
exit /b %errorlevel%

rem Runs a Python tool when Python is available.
:python
where python >nul 2>nul
if errorlevel 1 (
    echo Python is not on PATH; skipped %1.
    exit /b 0
)
python -I %*
exit /b %errorlevel%
