# Generates stage_clear.uf2: a single UF2 block writing 256 zero bytes over
# the SD-update staging header page (flash 0x100000 — keep in sync with
# FL_STAGE_HDR_OFFSET in flash_layout.h). Appended to the merged BOOTSEL
# image so ANY full reflash atomically disarms a pending staged update —
# without this, the flashloader would re-apply the old staged image right
# over a BOOTSEL rollback on the next boot.
param([string]$Out = "stage_clear.uf2")

$ErrorActionPreference = "Stop"
$b = New-Object byte[] 512
# Hex strings, not literals: PS 5.1 wraps hex literals > 0x7FFFFFFF to
# negative int32, which a [uint32] parameter rejects.
function W32([int]$off, [string]$hex) {
    $v = [Convert]::ToUInt32($hex, 16)
    [Array]::Copy([BitConverter]::GetBytes($v), 0, $b, $off, 4)
}
W32 0   "0A324655"   # magicStart0
W32 4   "9E5D5157"   # magicStart1
W32 8   "00002000"   # flags: familyID present
W32 12  "10100000"   # targetAddr: staging header page
W32 16  "100"        # payloadSize 256 (data stays all-zero)
W32 20  "0"          # blockNo
W32 24  "1"          # numBlocks
W32 28  "E48BFF56"   # familyID: RP2040
W32 508 "0AB16F30"   # magicEnd

[IO.File]::WriteAllBytes($Out, $b)
