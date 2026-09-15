# Publish a firmware release to GitHub and apply the release-retention rules.
#
# Publishing (skipped with -RetentionOnly): tags vM.m.p (if missing), creates
# the GitHub release and uploads the standard asset set produced by
# build_ota.ps1 + build_lite.ps1:
#   manifest-rp2350.json                          (fixed name - app discovery)
#   MicroPicoDrive_v<M.m.p>.uf2 / .bin / .json
#   partition_table.uf2, wifi_firmware.uf2        (first install)
#   MicroPicoDrive-Lite_v<M.m.p>_SD-update.uf2
#   MicroPicoDrive-Lite_v<M.m.p>_BOOTSEL-full.uf2 (from the Lite build tree)
#
# Retention rules (semver-aware; tags are NEVER deleted, only releases):
#   1. Within a minor line, only the newest patch survives.
#   2. The final release of the previous two minor lines is kept (rollback
#      anchors for SD:/.update downgrades).
#   3. The final release of each older major is kept forever.
# Deletions are DRY-RUN unless -Apply is passed.
#
# Usage:
#   powershell -File tools\publish_release.ps1                 # version from manifest, publish + dry-run retention
#   powershell -File tools\publish_release.ps1 -Apply          # ...and actually delete per the rules
#   powershell -File tools\publish_release.ps1 -RetentionOnly -Apply
#   powershell -File tools\publish_release.ps1 -NotesFile notes.md
param(
    [int]$Major = -1,          # default: read from dist\mainline-rp2350\manifest-rp2350.json
    [int]$Minor = -1,
    [int]$Patch = -1,
    [string]$NotesFile,        # markdown release notes; a stock body otherwise
    [switch]$RetentionOnly,    # skip publishing, only evaluate/apply retention
    [switch]$Apply             # actually delete releases the rules retire
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = "arleybls/micropicodrive-ng-firmware"
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root "dist"
$api  = "https://api.github.com/repos/$repo"

# GitHub token from the stored git credential (Git Credential Manager). The
# query goes through a temp file because PS 5.1 mangles piped native stdin;
# the file holds no secret and is blanked immediately after.
function Get-GithubHeaders {
    $q = Join-Path $env:TEMP "ghcredq.txt"
    [IO.File]::WriteAllText($q, "protocol=https`nhost=github.com`n`n")
    $cred = cmd /c "git credential fill < `"$q`""
    [IO.File]::WriteAllText($q, "")
    $token = ($cred | Where-Object { $_ -match '^password=' }) -replace '^password=', ''
    if (-not $token) { throw "no stored GitHub credential (git credential fill)" }
    return @{ Authorization = "Bearer $token"; Accept = "application/vnd.github+json" }
}
$h = Get-GithubHeaders

if ($Major -lt 0) {
    $man = Get-Content (Join-Path $dist "mainline-rp2350\manifest-rp2350.json") -Raw | ConvertFrom-Json
    $Major = $man.major; $Minor = $man.minor; $Patch = $man.patch
}
$ver = "$Major.$Minor.$Patch"
$tag = "v$ver"

if (-not $RetentionOnly) {
    $assets = @(
        @{ p = "$dist\mainline-rp2350\manifest-rp2350.json";        t = "application/json";         n = "manifest-rp2350.json" },
        @{ p = "$dist\mainline-rp2350\MicroPicoDrive_v$ver.json";   t = "application/json";         n = "MicroPicoDrive_v$ver.json" },
        @{ p = "$dist\mainline-rp2350\MicroPicoDrive_v$ver.bin";    t = "application/octet-stream"; n = "MicroPicoDrive_v$ver.bin" },
        @{ p = "$dist\mainline-rp2350\MicroPicoDrive_v$ver.uf2";    t = "application/octet-stream"; n = "MicroPicoDrive_v$ver.uf2" },
        @{ p = "$dist\mainline-rp2350\partition_table.uf2";         t = "application/octet-stream"; n = "partition_table.uf2" },
        @{ p = "$dist\mainline-rp2350\wifi_firmware.uf2";           t = "application/octet-stream"; n = "wifi_firmware.uf2" },
        @{ p = "$dist\MicroPicoDrive-Lite_v${ver}_SD-update.uf2";   t = "application/octet-stream"; n = "MicroPicoDrive-Lite_v${ver}_SD-update.uf2" },
        @{ p = "$env:TEMP\mpd-lite\MicroPicoDrive-full.uf2";        t = "application/octet-stream"; n = "MicroPicoDrive-Lite_v${ver}_BOOTSEL-full.uf2" }
    )
    foreach ($a in $assets) {
        if (-not (Test-Path $a.p)) { throw "missing asset: $($a.p) - run build_ota.ps1 / build_lite.ps1 for v$ver first" }
    }
    # Sanity: the manifest must describe the version being released.
    $m = Get-Content $assets[0].p -Raw | ConvertFrom-Json
    if ("$($m.major).$($m.minor).$($m.patch)" -ne $ver) {
        throw "manifest-rp2350.json says v$($m.major).$($m.minor).$($m.patch), not v$ver - rebuild first"
    }

    if (-not (git -C $root tag -l $tag)) {
        git -C $root tag -a $tag -m $tag
        if ($LASTEXITCODE -ne 0) { throw "git tag failed" }
    }
    git -C $root push origin $tag
    if ($LASTEXITCODE -ne 0) { throw "git push of tag failed" }

    if ($NotesFile) { $notes = Get-Content $NotesFile -Raw }
    else { $notes = "Firmware v$ver for MicroPicoDrive (Pico 2 W) and MicroPicoDrive-Lite (Pico). See commit history since the previous tag for changes." }

    $body = @{ tag_name = $tag; name = $tag; body = $notes; draft = $false; prerelease = $false } | ConvertTo-Json
    $rel = Invoke-RestMethod -Headers $h -Method Post -Uri "$api/releases" -Body $body -ContentType "application/json"
    Write-Host "release created: $($rel.html_url)" -ForegroundColor Green
    foreach ($a in $assets) {
        $null = Invoke-RestMethod -Headers $h -Method Post `
            -Uri "https://uploads.github.com/repos/$repo/releases/$($rel.id)/assets?name=$($a.n)" `
            -InFile $a.p -ContentType $a.t
        Write-Host "  uploaded $($a.n)"
    }
}

# ── Retention ────────────────────────────────────────────────────────────────
$rels = Invoke-RestMethod -Headers $h -Uri "$api/releases?per_page=100"
$parsed = @()
foreach ($r in $rels) {
    if ($r.tag_name -match '^v(\d+)\.(\d+)\.(\d+)$') {
        $parsed += [pscustomobject]@{
            id = $r.id; tag = $r.tag_name
            maj = [int]$Matches[1]; min = [int]$Matches[2]; pat = [int]$Matches[3]
            key = [int]$Matches[1] * 1000000 + [int]$Matches[2] * 1000 + [int]$Matches[3]
        }
    } else {
        Write-Host "retention: skipping unparseable tag '$($r.tag_name)' (kept)"
    }
}
if (-not $parsed) { Write-Host "retention: no releases"; return }

$latest = $parsed | Sort-Object key -Descending | Select-Object -First 1
$keep = @{}; $why = @{}

# Rule 1: newest patch per minor line; older patches retire.
$finals = @()
$parsed | Group-Object { "$($_.maj).$($_.min)" } | ForEach-Object {
    $f = $_.Group | Sort-Object key -Descending | Select-Object -First 1
    $finals += $f
}
# Rule 2: latest + the final of the previous two minor lines in latest's major.
$keep[$latest.id] = $true; $why[$latest.id] = "latest"
$anchors = $finals | Where-Object { $_.maj -eq $latest.maj -and $_.min -lt $latest.min } |
    Sort-Object key -Descending | Select-Object -First 2
foreach ($a in $anchors) { $keep[$a.id] = $true; $why[$a.id] = "minor-line anchor" }
# Rule 3: the final release of each older major.
$finals | Where-Object { $_.maj -lt $latest.maj } | Group-Object maj | ForEach-Object {
    $f = $_.Group | Sort-Object key -Descending | Select-Object -First 1
    $keep[$f.id] = $true; $why[$f.id] = "final of major $($_.Name)"
}

Write-Host "`n--- retention (rules: newest patch per minor; latest + 2 minor anchors; final of each older major) ---"
foreach ($p in ($parsed | Sort-Object key -Descending)) {
    if ($keep[$p.id]) { Write-Host ("KEEP    {0}  ({1})" -f $p.tag, $why[$p.id]) }
    else {
        if ($Apply) {
            Invoke-RestMethod -Headers $h -Method Delete -Uri "$api/releases/$($p.id)" | Out-Null
            Write-Host ("DELETED {0}  (tag kept)" -f $p.tag) -ForegroundColor Yellow
        } else {
            Write-Host ("DELETE  {0}  (dry run - pass -Apply)" -f $p.tag) -ForegroundColor Yellow
        }
    }
}
