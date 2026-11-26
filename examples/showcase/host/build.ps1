$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

$root = "f:\Projects\weft"
$bld = "$root\build\windows-msvc"
$outDir = "$root\artifacts\local\showcase-host"
New-Item -ItemType Directory -Force $outDir | Out-Null

$libs = @(
  "$bld\src\core\Release\weft_core.lib",
  "$bld\src\view\Release\weft_view.lib",
  "$bld\src\render\backends\d3d11\Release\weft_scene_d3d11.lib",
  "$bld\src\render\backends\d3d11\Release\weft_render_d3d11.lib",
  "$bld\src\platform\win32\Release\weft_platform_win32.lib",
  "$bld\src\script\Release\weft_script.lib",
  "$bld\src\canvas\Release\weft_canvas.lib",
  "$bld\third_party\quickjs-ng\Release\qjs.lib",
  "$bld\src\bridge\Release\weft_bridge.lib",
  "$bld\Release\weft_lodepng.lib",
  "$bld\Release\weft_jpeg.lib",
  "$bld\third_party\libwebp\Release\libwebpdemux.lib",
  "$bld\third_party\libwebp\Release\libwebp.lib",
  "$bld\third_party\libwebp\Release\libsharpyuv.lib",
  "$bld\src\scene\Release\weft_scene_d2d.lib",
  "$bld\src\scene\Release\weft_scene_native.lib",
  "$bld\src\scene\Release\weft_scene.lib",
  "$bld\src\layout\Release\weft_layout.lib",
  "$bld\src\text\Release\weft_text.lib",
  "$bld\Release\weft_unicode_case.lib",
  "$bld\src\style\Release\weft_style.lib",
  "$bld\src\dom\Release\weft_dom.lib",
  "$bld\src\text\Release\weft_bidi.lib",
  "$bld\Release\weft_sheenbidi.lib",
  "$bld\third_party\harfbuzz\Release\harfbuzz.lib",
  "$bld\third_party\yoga\Release\yogacore.lib",
  "$bld\Release\weft_unibreak.lib",
  "$bld\src\render\Release\weft_render.lib",
  "$bld\src\vector\Release\weft_vector.lib",
  "$bld\src\paint\Release\weft_paint.lib",
  "$bld\src\base\Release\weft_base.lib",
  "$bld\third_party\freetype\Release\freetype.lib",
  "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "d2d1.lib", "dwrite.lib",
  "windowscodecs.lib", "shlwapi.lib", "ole32.lib", "user32.lib", "gdi32.lib", "advapi32.lib"
) -join " "

$cmd = "`"$vcvars`" && cl /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /I`"$root\include`" `"$root\examples\showcase\host\showcase_host.cpp`" /Fo:`"$outDir\showcase_host.obj`" /Fe:`"$outDir\showcase_host.exe`" /link /STACK:8388608 $libs"
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built $outDir\showcase_host.exe successfully."
