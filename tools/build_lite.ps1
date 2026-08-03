# Build the Lite (RP2040) firmware from the single tree.
#
# New with the single-tree fold. Before it, the Lite image was built by hand
# (or by the VS Code Pico extension) and the board came from the CMakeLists
# default — which the fold removes. PICO_BOARD is a CACHE variable and the
# build dir persists, so passing it explicitly is not optional: a dir last
# configured for pico2_w would otherwise emit an RP2350 image under Lite
# filenames.
#
# Produces, in dist\:
#   MicroPicoDrive-Lite_v<M.m.p>_SD-update.uf2  - app-only, for SD:/.update
# and leaves in the build dir:
#   MicroPicoDrive-full.uf2  - flashloader + app + stage_clear, the ONLY
#                              correct USB BOOTSEL / first-install image
#                              (copy it to the RPI-RP2 drive)
#
# Usage:  powershell -File tools\build_lite.ps1 [-Major 2] [-Minor 1] [-Patch 0]
param(
    [int]$Major = 2,
    [int]$Minor = 1,
    [int]$Patch = 0
)
# Same 0-99 ceiling as mainline. Lite has no A/B seal so nothing forces it,
# but the two boards must not develop divergent version semantics now that
# they share a tree (study trap T5 / IMPROVEMENTS 7b).
if ($Patch -lt 0 -or $Patch -gt 99) { throw "Patch must be 0-99" }
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$sdk  = "$env:USERPROFILE\.pico-sdk"
$env:PATH = "$sdk\cmake\v3.31.5\bin;$sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$sdk\sdk\2.2.0"
# Short out-of-tree build dir: this tree's path risks the SDK object-path limit.
$build = Join-Path $env:TEMP "mpd-lite"
# dist\ root is the Lite home; mainline artifacts live in dist\mainline-rp2350.
$dist  = Join-Path $root "dist"
New-Item -ItemType Directory -Force $dist | Out-Null

$ver = "$Major.$Minor.$Patch"
Write-Host "=== Building Lite v$ver ===" -ForegroundColor Cyan
cmake -S $root -B $build -G Ninja "-DPICO_BOARD=pico" `
    "-DMPD_FW_VERSION_MAJOR=$Major" "-DMPD_FW_VERSION_MINOR=$Minor" `
    "-DMPD_FW_VERSION_PATCH=$Patch" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# Guard against a stale cache silently producing the other board's image.
$cachedBoard = (Select-String -Path "$build\CMakeCache.txt" -Pattern '^PICO_BOARD:').Line
if ($cachedBoard -notmatch 'pico$') { throw "build dir is configured for the wrong board: $cachedBoard" }

$sdUf2 = Join-Path $dist "MicroPicoDrive-Lite_v${ver}_SD-update.uf2"
Copy-Item "$build\MicroPicoDrive.uf2" $sdUf2 -Force

$full = "$build\MicroPicoDrive-full.uf2"
if (-not (Test-Path $full)) { throw "merged BOOTSEL image missing - the flashloader half of CMakeLists did not run" }

Write-Host "`nDone." -ForegroundColor Green
Write-Host "  SD update : $sdUf2 ($((Get-Item $sdUf2).Length) bytes)"
Write-Host "  BOOTSEL   : $full ($((Get-Item $full).Length) bytes)"
Write-Host "`nInstall:"
Write-Host "  SD      : copy the SD-update file above into \.update\ on the card, then power on"
Write-Host "  BOOTSEL : hold BOOTSEL while plugging in, then copy the merged image above to RPI-RP2"
