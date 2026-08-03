// Minimal sd_menu API surface for the vendored sandbox modules (sd_check,
// sd_thumb, UserInterfaceExtension path title). The sandbox's sd_menu.c
// browser is NOT vendored — MicroPicoDrive keeps its own directory browser in
// UserInterface.c, which implements these two functions instead.
// Sandbox reference: c:\VSCode Projects\UserInterfaceExtension_SPI\sd_menu.h
// (commit 057b1ae).
#ifndef SD_MENU_H
#define SD_MENU_H

#include <stdbool.h>
#include "ff.h"

// Mount the SD volume (shared FATFS); FR_OK on success.
FRESULT sd_fs_mount(void);

// True if the card is (now) mounted — retries the mount on each call so a
// card inserted after boot is picked up (sandbox contract; ota_ble.c uses it).
bool sd_menu_available(void);

// MicroPicoDrive guard for the vendored BLE server (BLE_PROTOCOL.md §5b):
// true while a cartridge is mounted or the QL is actively using the drive —
// SD mutations and the reboot op must answer EBUSY then.
bool cartridge_busy(void);

// Current browse directory ("/" at root) for sidecar/thumb path building.
const char *sd_menu_cur_path(void);

// Cheap "is the card still there?" probe (one SPI status command, not a
// mount) — for periodic polling while a screen is up.
bool sd_menu_card_present(void);

#endif
