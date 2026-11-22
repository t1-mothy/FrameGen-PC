# Downloads the NVIDIA Streamline SDK (DLSS Frame Generation) into third_party\streamline.
# Streamline is MIT licensed; the DLSS-G / NGX binaries inside are covered by NVIDIA's licence
# (see third_party\streamline\license.txt and *.license.txt). Not committed to the repository.
param([string]$Version = "v2.14.16")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dl = Join-Path $root "third_party\dl"
$dst = Join-Path $root "third_party\streamline"
New-Item -ItemType Directory -Force $dl | Out-Null
$zip = Join-Path $dl "streamline-sdk-$Version.zip"
if (-not (Test-Path $zip)) {
    $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/$Version/streamline-sdk-$Version.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
Expand-Archive -Path $zip -DestinationPath $dst
Write-Host "Streamline $Version is in $dst — run .\build.ps1 to build with DLSS Frame Generation support."
