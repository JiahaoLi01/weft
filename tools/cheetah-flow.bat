@echo off
setlocal EnableExtensions
rem CaseCheetah 4K flow window runner: the same React page drawn by D3D11 or by Vulkan. Double-click for the menu, or
rem pass the entry number:
rem   tools\cheetah-flow.bat              menu
rem   tools\cheetah-flow.bat 2            Vulkan window
rem   tools\cheetah-flow.bat 3 novsync    benchmark with vertical sync off
rem Each run writes to artifacts\local\cheetah-flow-runs\<time>-<mode>. The Release probe is built when it is missing;
rem entry 5 rebuilds it after source changes. The Vulkan entries need a Vulkan driver; the window title shows the GPU.

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
echo   CaseCheetah flow window - Release build, hardware GPU
echo.
echo   1  D3D11 window       4K flow page with motion; wheel scrolls the list, closing the window finishes
echo   2  Vulkan window      the same page through the Vulkan renderer's C ABI and a Vulkan swapchain
echo   3  Benchmark          hidden 4K wheel run on D3D11, Vulkan, Vulkan without validation; about 2 minutes
echo   4  Vulkan self-test   hidden 4K run with input, scroll, resize and pixel checks
echo   5  Build              Release weft_d3d11_probe
echo   V  VSync %VSYNC_STATE%            for entries 1 to 3
echo.
set "CHOICE="
set /p "CHOICE=Choose 1-5, or V to toggle VSync: "
if /i "%CHOICE%"=="V" (
    if defined VSYNC (set "VSYNC=") else (set "VSYNC=--no-vsync")
    goto menu
)

:dispatch
set "TAG="
if defined VSYNC set "TAG=-novsync"
if "%CHOICE%"=="5" goto build
if not exist "%EXE%" (
    call :build_probe
    if errorlevel 1 goto finish
)
if "%CHOICE%"=="1" goto d3d11
if "%CHOICE%"=="2" goto vulkan
if "%CHOICE%"=="3" goto benchmark
if "%CHOICE%"=="4" goto selftest
echo Unknown entry "%CHOICE%".
set "CODE=1"
goto done

:build
call :build_probe
goto finish

:d3d11
set "OUT=artifacts\local\cheetah-flow-runs\%STAMP%-d3d11%TAG%"
"%EXE%" --cheetah-flow-window-hardware "%OUT%" %VSYNC%
goto finish

:vulkan
set "OUT=artifacts\local\cheetah-flow-runs\%STAMP%-vulkan%TAG%"
"%EXE%" --cheetah-flow-window-vulkan "%OUT%" %VSYNC%
goto finish

:benchmark
set "OUT=artifacts\local\cheetah-flow-runs\%STAMP%-benchmark%TAG%"
call :python tools\benchmark_flow_window_apis.py "%EXE%" "%OUT%" 3 %VSYNC%
goto finish

:selftest
set "OUT=artifacts\local\cheetah-flow-runs\%STAMP%-vulkan-selftest"
"%EXE%" --cheetah-flow-window-test-4k-vulkan "%OUT%"
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
    echo Python is not on PATH; the benchmark needs it.
    exit /b 1
)
python -I %*
exit /b %errorlevel%
