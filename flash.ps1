# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Evergo-Mainline
#
# Build, flash and open the serial monitor for the evergo reboot helper.
#   powershell -ExecutionPolicy Bypass -File flash.ps1 [-Port COM5] [-IdfProfile <path>]
param(
    [string]$Port = "",
    [string]$IdfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$proj = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $IdfProfile)) {
    Write-Host "ESP-IDF export script not found: $IdfProfile" -ForegroundColor Red
    Write-Host "Install ESP-IDF v6.1 or pass -IdfProfile <path to the export/profile script>."
    exit 1
}
if (-not (Test-Path (Join-Path $proj 'main\wifi_config.h'))) {
    Write-Host "main\wifi_config.h not found." -ForegroundColor Yellow
    Write-Host "Copy main\wifi_config.h.example to main\wifi_config.h and fill in your WiFi."
    exit 1
}

. $IdfProfile | Out-Null
Set-Location $proj

if (-not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if (-not $ports) { Write-Host "no serial port found - plug the ESP32 in" -ForegroundColor Red; exit 1 }
    $Port = $ports[0]
    Write-Host "using $Port" -ForegroundColor Cyan
}

idf.py -p $Port build flash monitor
