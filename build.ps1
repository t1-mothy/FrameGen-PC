# Builds FrameGen with the Visual Studio C++ tools (2022 or newer, Build Tools are enough).
#   .\build.ps1            → build\FrameGen.exe and build\framegen-cli.exe (Release)
#   .\build.ps1 -Debug     → Debug build in build-debug\
#   .\build.ps1 -Clean     → delete the build folder first
param([switch]$Debug, [switch]$Clean, [string]$Target = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer not found. Install Visual Studio 2022+ (or Build Tools) with "Desktop development with C++".' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ x64 tools found. Add the "Desktop development with C++" workload.' }
$config = if ($Debug) { 'Debug' } else { 'Release' }
$dir = if ($Debug) { 'build-debug' } else { 'build' }
if ($Clean -and (Test-Path "$root\$dir")) { Remove-Item -Recurse -Force "$root\$dir" }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"   # vcvars looks for vswhere.exe on PATH
$cmd = "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$root\$dir`" -G Ninja -DCMAKE_BUILD_TYPE=$config >nul && cmake --build `"$root\$dir`"$(if ($Target) { " --target $Target" })"
cmd /c $cmd 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
Write-Host "Built $root\$dir\FrameGen.exe"
