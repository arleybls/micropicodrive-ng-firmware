// Flash map shared by the flashloader and the app (SD-update feature).
//
//   0x000000 - 0x004000  flashloader (boot2 + staged-image applier;
//                        never rewritten after install — update via BOOTSEL only)
//   0x004000 - 0x100000  app (MicroPicoDrive-Lite, linked at XIP_BASE+0x4000
//                        by memmap_app.ld — LENGTH there must match FL_APP_MAX_SIZE)
//   0x100000 - 0x1FF000  staging area: header sector + image (written by
//                        sd_update_lite.c, applied by the flashloader)
//   0x1FF000 - 0x200000  display-settings sector (UserInterface.c)
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include <stdint.h>

#define FL_APP_OFFSET       0x4000u
#define FL_APP_MAX_SIZE     (0x100000u - FL_APP_OFFSET)

#define FL_STAGE_HDR_OFFSET 0x100000u             // one 4 KB sector
#define FL_STAGE_IMG_OFFSET 0x101000u
#define FL_STAGE_MAX_SIZE   (0x1FF000u - FL_STAGE_IMG_OFFSET)

#define FL_SETTINGS_OFFSET  0x1FF000u             // display-settings sector

#define FL_STAGE_MAGIC      0x4C465055u           // "UPFL"

// The regions must tile exactly (memmap_app.ld and tools/make_stage_clear.ps1
// carry hand-synced copies of these numbers — see their comments). The
// PICO_FLASH_SIZE_BYTES assert turns "built for a board with a different
// flash size" into a compile error instead of a settings sector floating
// over the staging area.
_Static_assert(FL_APP_OFFSET + FL_APP_MAX_SIZE == FL_STAGE_HDR_OFFSET,
               "app region must end where the staging header starts");
_Static_assert(FL_STAGE_HDR_OFFSET + 0x1000u == FL_STAGE_IMG_OFFSET,
               "staging header is exactly one 4 KB sector");
_Static_assert(FL_STAGE_IMG_OFFSET + FL_STAGE_MAX_SIZE == FL_SETTINGS_OFFSET,
               "staging area must end where the settings sector starts");
#ifdef PICO_FLASH_SIZE_BYTES
_Static_assert(FL_SETTINGS_OFFSET + 0x1000u == PICO_FLASH_SIZE_BYTES,
               "flash map assumes a 2 MB part");
#endif

// Header page at FL_STAGE_HDR_OFFSET. Written LAST by the stager: a valid
// magic+crc means the staged image is complete. The flashloader applies it
// whenever it differs from the app region, then ERASES the header once the
// app matches — that consume step is what stops a later BOOTSEL rollback
// from being silently re-overwritten by the same staged image (see
// flashloader.c; the merged UF2's stage_clear segment covers the reflash
// itself).
typedef struct {
    uint32_t magic;
    uint32_t size;      // staged image bytes (multiple of 256)
    uint32_t crc32;     // fl_crc32 over the staged image
    uint32_t reserved;
} fl_stage_hdr_t;

// Small bitwise CRC-32 (IEEE, reflected) — shared so the stager and the
// flashloader can never disagree. ~8 cycles/bit is fine for <1 MB images.
static inline uint32_t fl_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static inline uint32_t fl_crc32(const uint8_t *p, uint32_t n) {
    return ~fl_crc32_update(0xFFFFFFFFu, p, n);
}

#endif // FLASH_LAYOUT_H
