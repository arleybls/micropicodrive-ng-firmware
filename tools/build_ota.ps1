# Build OTA-ready firmware packages. Adapted from the UIExt sandbox
# tools\build_ota.ps1 (commit 057b1ae) for MicroPicoDrive: program name,
# MPD_FW_VERSION_* cache vars, v2.x numbering, dedicated build\ota tree.
#
# Produces, in dist\:
#   partition_table.uf2          - flash ONCE via USB BOOTSEL (first install,
#                                  before the firmware image itself)
#   wifi_firmware.uf2            - CYW43 radio firmware (first install)
#   MicroPicoDrive_v<M.m>.uf2    - USB-flashable firmware (sealed + versioned)
#   MicroPicoDrive_v<M.m>.bin    - raw image for BLE OTA upload / SD .update
#   MicroPicoDrive_v<M.m>.json   - manifest {major, minor, size, sha256}
# and the same three files for version <M.m+1>, so an OTA upload can be
# tested immediately (device only accepts strictly newer versions over BLE).
#
# Usage:  powershell -File tools\build_ota.ps1 [-Major 2] [-Minor 0] [-Patch 0]
# Three-part versions since v2.5.1. The sealed IMAGE_DEF and the BLE wire
# only carry major/minor u16, so ordering there uses minor*100+patch
# (patch 0-99); displays and filenames use the full M.m.p.
param(
    [int]$Major = 2,
    [int]$Minor = 0,
    [int]$Patch = 0
)
if ($Patch -lt 0 -or $Patch -gt 98) { throw "Patch must be 0-98 (99 is reserved for the auto OTA-test build)" }
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$sdk  = "$env:USERPROFILE\.pico-sdk"
$env:PATH = "$sdk\cmake\v3.31.5\bin;$sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$sdk\sdk\2.2.0"
$picotool = "$sdk\picotool\2.2.0-a4\picotool\picotool.exe"
$objcopy  = "$sdk\toolchain\14_2_Rel1\bin\arm-none-eabi-objcopy.exe"
# Short out-of-tree build dir: the in-tree build\ota carried a stale CMake
# cache into the tree copy (pointing at the original repo's paths), and this
# tree's longer path risks the SDK object-path limit anyway.
$build = Join-Path $env:TEMP "mpd-ota"
# dist\ root is Lite-only since 2026-07-28; Pico 2 W artifacts live here.
$dist  = Join-Path $root "dist\mainline-rp2350"
New-Item -ItemType Directory -Force $dist | Out-Null

function BuildOne([int]$maj, [int]$min, [int]$pat) {
    Write-Host "=== Building v$maj.$min.$pat ===" -ForegroundColor Cyan
    # -DPICO_BOARD is EXPLICIT since the single-tree fold: one tree now builds
    # both products, PICO_BOARD is a CACHE variable, and $build persists — so
    # relying on the CMakeLists default would let a stale dir emit the wrong
    # board's image under this script's filenames.
    cmake -S $root -B $build -G Ninja "-DPICO_BOARD=pico2_w" `
        "-DMPD_FW_VERSION_MAJOR=$maj" "-DMPD_FW_VERSION_MINOR=$min" `
        "-DMPD_FW_VERSION_PATCH=$pat" | Out-Null
    cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    $ver    = "$maj.$min.$pat"
    $elf    = "$build\MicroPicoDrive.elf"
    $sealed = "$dist\MicroPicoDrive_v$ver.elf"
    $uf2    = "$dist\MicroPicoDrive_v$ver.uf2"
    $bin    = "$dist\MicroPicoDrive_v$ver.bin"
    $man    = "$dist\MicroPicoDrive_v$ver.json"

    # Seal: adds IMAGE_DEF hash + version — this is what the RP2350 bootrom
    # compares when picking the newest valid A/B slot. Minor carries the
    # combined minor*100+patch ordering value.
    $sealMinor = $min * 100 + $pat
    & $picotool seal --quiet --hash $elf $sealed --major $maj --minor $sealMinor
    if ($LASTEXITCODE -ne 0) { throw "picotool seal failed" }
    & $picotool uf2 convert --quiet $sealed $uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool uf2 convert failed" }
    & $objcopy -O binary $sealed $bin
    if ($LASTEXITCODE -ne 0) { throw "objcopy failed" }

    $sha  = (Get-FileHash -Algorithm SHA256 $bin).Hash.ToLower()
    $size = (Get-Item $bin).Length
    @{ major = $maj; minor = $min; patch = $pat; size = $size; sha256 = $sha } |
        ConvertTo-Json | Out-File -Encoding ascii $man
    Remove-Item $sealed
    Write-Host "  -> $uf2 ($size bytes, sha256 $($sha.Substring(0,12))...)"
}

# Partition table (first-install only)
& $picotool partition create --quiet "$root\boards\partition_table.json" "$dist\partition_table.uf2"
if ($LASTEXITCODE -ne 0) { throw "picotool partition create failed" }
Write-Host "partition_table.uf2 written"

BuildOne $Major $Minor $Patch
BuildOne $Major $Minor ($Patch + 1)   # strictly-newer set for OTA testing

# CYW43 Wi-Fi/BT firmware for its dedicated partition (first-install only;
# built by pico_use_wifi_firmware_partition alongside the app)
Copy-Item "$build\MicroPicoDrive_wifi_firmware.uf2" "$dist\wifi_firmware.uf2" -Force
Write-Host "wifi_firmware.uf2 written"

# Leave the build tree back on the base version
cmake -S $root -B $build -G Ninja "-DPICO_BOARD=pico2_w" `
    "-DMPD_FW_VERSION_MAJOR=$Major" "-DMPD_FW_VERSION_MINOR=$Minor" `
    "-DMPD_FW_VERSION_PATCH=$Patch" | Out-Null

Write-Host "`nDone. dist\ contains:" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, Length -AutoSize
