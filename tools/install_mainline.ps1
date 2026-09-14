# First install of the mainline (Pico 2 W / RP2350) firmware over USB BOOTSEL,
# driven by picotool. A blank unit needs three images, in order:
#   1. partition_table.uf2  - then a reboot back into BOOTSEL so the bootrom
#                             re-reads the table (partition-addressed loads
#                             would otherwise land nowhere)
#   2. wifi_firmware.uf2    - CYW43 radio firmware, dedicated partition
#   3. MicroPicoDrive_v<M.m.p>.uf2 - sealed app image into slot A
# The app version defaults to the release manifest (manifest-rp2350.json), NOT
# the newest file in dist\ - build_ota.ps1 also emits a strictly-newer .patch+1
# image meant only for OTA testing.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\install_mainline.ps1
#   powershell -ExecutionPolicy Bypass -File tools\install_mainline.ps1 -Version 2.8.0
#
# Hold BOOTSEL while plugging the Pico 2 W in; the script waits for it.
param(
    [string]$Version,   # e.g. "2.8.0"; default: version in manifest-rp2350.json
    [string]$Dist = (Join-Path (Split-Path $PSScriptRoot -Parent) "dist\mainline-rp2350")
)
$ErrorActionPreference = "Stop"

$picotool = "$env:USERPROFILE\.pico-sdk\picotool\2.2.0-a4\picotool\picotool.exe"
if (-not (Test-Path $picotool)) { throw "picotool not found: $picotool" }

if (-not $Version) {
    $manifest = Join-Path $Dist "manifest-rp2350.json"
    if (-not (Test-Path $manifest)) { throw "manifest not found: $manifest (build first: tools\build_ota.ps1)" }
    $m = Get-Content $manifest -Raw | ConvertFrom-Json
    $Version = "$($m.major).$($m.minor).$($m.patch)"
}
$partTable = Join-Path $Dist "partition_table.uf2"
$wifi      = Join-Path $Dist "wifi_firmware.uf2"
$app       = Join-Path $Dist "MicroPicoDrive_v$Version.uf2"
foreach ($f in @($partTable, $wifi, $app)) {
    if (-not (Test-Path $f)) { throw "image not found: $f (build first: tools\build_ota.ps1)" }
}

# Runs picotool with native stderr folded into plain output lines (PS 5.1
# would otherwise wrap them in ErrorRecords and trip ErrorActionPreference).
# Output goes straight to the console via Write-Host so the function's return
# value is ONLY the exit code (pipeline output would pollute it).
function Invoke-Picotool([string[]]$ToolArgs) {
    $quoted = ($ToolArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    cmd /c "`"$picotool`" $quoted 2>&1" | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}

function Wait-Bootsel([string]$why) {
    Write-Host $why
    while ($true) {
        cmd /c "`"$picotool`" info > nul 2>&1"
        if ($LASTEXITCODE -eq 0) { break }
        Start-Sleep -Seconds 1
    }
}

Wait-Bootsel "Waiting for a Pico 2 W in BOOTSEL mode (hold BOOTSEL while plugging in)..."

Write-Host "`nInstalling MicroPicoDrive v$Version from $Dist"

Write-Host "`n[1/3] Partition table..."
if ((Invoke-Picotool @("load", "-v", $partTable)) -ne 0) { throw "partition table load failed" }
if ((Invoke-Picotool @("reboot", "-u")) -ne 0) { throw "reboot into BOOTSEL failed" }
Start-Sleep -Seconds 2
Wait-Bootsel "Waiting for the device to re-enter BOOTSEL with the new partition table..."

Write-Host "`n[2/3] CYW43 radio firmware..."
if ((Invoke-Picotool @("load", "-v", $wifi)) -ne 0) { throw "wifi firmware load failed" }

Write-Host "`n[3/3] Application v$Version..."
if ((Invoke-Picotool @("load", "-v", $app)) -ne 0) { throw "app load failed" }

Write-Host "`nFlash contents:"
$null = Invoke-Picotool @("info")

Write-Host "`nRebooting into the app..."
if ((Invoke-Picotool @("reboot")) -ne 0) { throw "picotool reboot failed" }

Write-Host "`nDone. First install complete: v$Version in slot A, slot B fills on the first OTA."
Write-Host "New unit reminder: pair this PC before any OTA (tools\ota_bletest.py pair)."
