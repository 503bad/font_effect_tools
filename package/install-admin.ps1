# Installs the Flame Text plugin into the system OBS install (C:\Program Files\obs-studio).
# Self-elevates to administrator. Copies BOTH the DLL and the data folder so OBS
# can find effects/ and locale/.
#
# Run: right-click -> "Run with PowerShell"  (a UAC prompt will appear)

# --- self-elevate ---
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        '-ExecutionPolicy','Bypass','-NoProfile','-File',"`"$PSCommandPath`""
    )
    return
}

$ErrorActionPreference = 'Stop'
$src    = Join-Path $PSScriptRoot 'effect-tools'
$obsDir = 'C:\Program Files\obs-studio'

$dllSrc  = Join-Path $src 'bin\64bit\effect-tools.dll'
$dllDst  = Join-Path $obsDir 'obs-plugins\64bit'
$dataSrc = Join-Path $src 'data'
$dataDst = Join-Path $obsDir 'data\obs-plugins\effect-tools'

Write-Host "Installing Flame Text plugin into: $obsDir`n"

New-Item -ItemType Directory -Force -Path $dllDst  | Out-Null
New-Item -ItemType Directory -Force -Path $dataDst | Out-Null

Copy-Item $dllSrc -Destination $dllDst -Force
Copy-Item -Path (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force

Write-Host "DLL  -> $dllDst\effect-tools.dll"      -ForegroundColor Green
Write-Host "data -> $dataDst\"                       -ForegroundColor Green
Write-Host ""
Write-Host "Done. Fully restart OBS (quit and reopen), then the Flame Text source will render." -ForegroundColor Green
Write-Host "Press Enter to close..."
[void][System.Console]::ReadLine()
