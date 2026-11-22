# Builds a release: dist\FrameGen-Setup-<version>.exe (per-user installer, no admin rights).
#   .\package.ps1
# NVIDIA VFG is included when its runtime is in third_party\nvvfx\runtime (see docs/VFG.md);
# otherwise the installer contains FrameGen only (VFG can still be pointed at in Settings).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
& "$root\build.ps1"
& "$root\build.ps1" -Target FrameGenSetup
$setup = Get-ChildItem "$root\build\FrameGen-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
Copy-Item $setup.FullName "$root\dist\" -Force
$vfg = Test-Path "$root\third_party\nvvfx\runtime\NVVideoEffects.dll"
Write-Host ("Installer: dist\{0} ({1:N0} MB){2}" -f $setup.Name, ($setup.Length / 1MB), $(if ($vfg) { ', includes NVIDIA VFG' } else { ', without NVIDIA VFG' }))
