# Copies the effect-tools plugin into the current user's OBS plugins folder.
# Run: right-click -> "Run with PowerShell", or:  powershell -ExecutionPolicy Bypass -File install-to-obs.ps1

$ErrorActionPreference = 'Stop'
$src  = Join-Path $PSScriptRoot 'effect-tools'
$dest = Join-Path $env:APPDATA 'obs-studio\plugins\effect-tools'

if (-not (Test-Path $src)) {
    Write-Host "ERROR: 'effect-tools' folder not found next to this script." -ForegroundColor Red
    exit 1
}

Write-Host "Source : $src"
Write-Host "Target : $dest"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Path (Join-Path $src '*') -Destination $dest -Recurse -Force

Write-Host ""
Write-Host "Done. Installed to:" -ForegroundColor Green
Write-Host "  $dest"
Write-Host "Restart OBS, then add a source named 'Flame Text' / '炎テキスト'."
