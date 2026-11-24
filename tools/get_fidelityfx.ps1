# Downloads AMD's signed FSR Frame Generation DLLs (AMD FidelityFX SDK) into third_party\fidelityfx\bin,
# plus AMD's licence. The SDK headers FrameGen needs are already in external\fidelityfx (MIT).
# AMD's licence allows redistributing the DLLs in binary form with its notice (shipped by the
# installer as amd-fsr\licenses\AMD-FidelityFX-license.md). Not committed to the repository.
param([string]$Version = "v2.4.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dst = Join-Path $root "third_party\fidelityfx"
New-Item -ItemType Directory -Force (Join-Path $dst "bin") | Out-Null
$base = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/$Version/Kits/FidelityFX"
foreach ($f in "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll") {
    Write-Host "Downloading $f"
    Invoke-WebRequest -Uri "$base/signedbin/$f" -OutFile (Join-Path $dst "bin\$f") -UseBasicParsing
    $sig = Get-AuthenticodeSignature (Join-Path $dst "bin\$f")
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'CN=Advanced Micro Devices') { throw "$f is not signed by AMD" }
}
Invoke-WebRequest -Uri "$base/docs/license.md" -OutFile (Join-Path $dst "license.md") -UseBasicParsing
Write-Host "AMD FSR $Version is in $dst — run .\build.ps1 (or .\package.ps1) to include it."
