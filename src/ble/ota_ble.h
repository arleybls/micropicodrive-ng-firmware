// BLE OTA firmware update — device side (see PLAN_OTA_UPDATE.md).
// The radio is only powered inside the blocking mode functions below; all of
// them return with advertising stopped and the BT controller powered off.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Firmware version (single source: CMake UIEXT_FW_VERSION_MAJOR/MINOR)
#ifndef UIEXT_FW_VERSION_MAJOR
#define UIEXT_FW_VERSION_MAJOR 0
#endif
#ifndef UIEXT_FW_VERSION_MINOR
#define UIEXT_FW_VERSION_MINOR 0
#endif
#ifndef UIEXT_FW_VERSION_PATCH
#define UIEXT_FW_VERSION_PATCH 0
#endif
// Ordering value carried where only major/minor u16 fit (BLE OP_START,
// manifest compare, picotool seal --minor): minor*100 + patch, patch 0-99.
#define UIEXT_FW_VERNUM_MINOR (UIEXT_FW_VERSION_MINOR * 100 + UIEXT_FW_VERSION_PATCH)

// "v1.0" — for menus and the BLE device-info characteristic.
const char *ota_fw_version_string(void);

// True if this boot came from an A/B partition (partition table installed).
// OTA and revert are unavailable without it.
bool ota_partition_boot(void);

// True if the inactive slot currently holds an image (revert possible).
bool ota_other_slot_has_image(void);

// True once if any BLE SD mutation (push/delete/rename/setlabel) landed since
// the last call — read-and-clear; the browser rebuilds its listing on it.
bool ota_sd_changed(void);

// App-initiated reboot (§5 "reboot" op): watchdog scratch magic telling the
// next boot to enter Connect mode directly. Survives the watchdog reset,
// cleared on power-on and after consumption (main()).
#define OTA_REBOOT_TO_CONNECT_MAGIC 0x4D504443u   // "MPDC"

// Blocking UI modes, called from the config menu. Each drives the display
// via the uiext_ota_* helpers and returns when done/cancelled/timed out.
void ota_run_pair_mode(void);
void ota_run_connect_mode(void);  // advertise + serve JSON browse/pull (companion app)
void ota_run_update_mode(void);
void ota_run_revert(void);
void ota_run_forget_bonds(void);
void ota_run_paired_list(void);   // browse bonds; SELECT offers to forget one

// Slot/flash primitives shared with the SD-card update path (sd_update_ota.c).
// No radio involvement; safe at boot.
bool ota_slots(uint32_t *active_off, uint32_t *target_off, uint32_t *target_size);
bool ota_flash_sector(uint32_t flash_off, const uint8_t *data, uint32_t len);
bool ota_erase_sector_at(uint32_t flash_off);
