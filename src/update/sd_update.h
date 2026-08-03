// Boot-time firmware update from the SD card's hidden /.update folder.
//
// ONE interface, TWO implementations — CMake compiles exactly one of them
// (see docs/SINGLE_TREE_STUDY.md §3c). They share nothing but this prototype:
//
//   sd_update_lite.c  (PICO_BOARD=pico)    RP2040, no A/B slots.
//       Input:      the first *.uf2 in SD:/.update
//       Validation: UF2 magic + family 0xE48BFF56 + CRC-32
//       Applies:    stages into the staging flash region, header written last,
//                   then reboots — the flashloader applies it on the way back
//                   up. See flash_layout.h.
//
//   sd_update_ota.c   (PICO_BOARD=pico2_w) RP2350, A/B partitions.
//       Input:      a build_ota.ps1 pair (*.bin + matching *.json) in /.update
//       Validation: SHA-256 against the manifest
//       Applies:    writes the INACTIVE slot with sector 0 held back until
//                   verified, then reboots into it.
//
// Consequence worth knowing: a card prepared for one board does nothing on
// the other. Both are offered at boot only when the SD version differs from
// the running one; No (or timeout) declines per each flavour's own rules.
//
// This is called UNCONDITIONALLY at boot on both boards (study trap T2): the
// call used to sit inside #if UIEXT_OTA_ENABLED, which would have removed SD
// updating from the RP2040 image entirely.
#pragma once

void sd_update_check_at_boot(void);
