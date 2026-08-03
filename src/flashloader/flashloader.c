// Flashloader: first-stage image at the start of flash (never rewritten in
// the field — it only changes via a BOOTSEL flash of the merged UF2).
//
// On every boot: if the staging area holds a complete, CRC-valid image that
// differs from the app region, copy it over the app sector-by-sector, then
// jump to the app at FL_APP_OFFSET. The copy is idempotent — power loss at
// any point just means the next boot re-runs it (this loader and the staged
// image are never erased during an apply).
//
// Kept deliberately dumb: no SD, no display, no stdio — so it never needs a
// field update itself.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/nvic.h"
#include "flash_layout.h"

static uint8_t sector_buf[FLASH_SECTOR_SIZE];

static void apply_staged(uint32_t size) {
    const uint8_t *staged = (const uint8_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET);
    uint32_t nsect = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;

    // IRQ handlers live in flash: no interrupts while XIP goes down.
    uint32_t save = save_and_disable_interrupts();
    for (uint32_t s = 0; s < nsect; s++) {
        uint32_t off = s * FLASH_SECTOR_SIZE;
        uint32_t n = (size - off < FLASH_SECTOR_SIZE) ? size - off : FLASH_SECTOR_SIZE;
        // Buffer the staged sector in RAM: flash is unreadable during the
        // erase/program calls below (which the SDK runs from RAM).
        memset(sector_buf, 0xFF, sizeof(sector_buf));
        memcpy(sector_buf, staged + off, n);
        flash_range_erase(FL_APP_OFFSET + off, FLASH_SECTOR_SIZE);
        flash_range_program(FL_APP_OFFSET + off, sector_buf, FLASH_SECTOR_SIZE);
    }
    restore_interrupts(save);
}

int main(void) {
    const fl_stage_hdr_t *hdr = (const fl_stage_hdr_t *)(XIP_BASE + FL_STAGE_HDR_OFFSET);

    if (hdr->magic == FL_STAGE_MAGIC &&
        hdr->size >= 256u && hdr->size <= FL_STAGE_MAX_SIZE &&
        hdr->size <= FL_APP_MAX_SIZE) {
        const uint8_t *staged = (const uint8_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET);
        const uint8_t *app    = (const uint8_t *)(XIP_BASE + FL_APP_OFFSET);
        if (fl_crc32(staged, hdr->size) == hdr->crc32) {
            if (memcmp(staged, app, hdr->size) != 0)
                apply_staged(hdr->size);
            // Consume the stage once the app matches: a later BOOTSEL
            // reflash (rollback) must never be overwritten by this image.
            // An interrupted apply keeps the header, so retry still works.
            if (memcmp(staged, app, hdr->size) == 0) {
                uint32_t save = save_and_disable_interrupts();
                flash_range_erase(FL_STAGE_HDR_OFFSET, FLASH_SECTOR_SIZE);
                restore_interrupts(save);
            }
        }
    }

    // Hand over to the app: vector table sits after the app's (unused) boot2.
    uint32_t *app_vectors = (uint32_t *)(XIP_BASE + FL_APP_OFFSET + 0x100u);

    // No plausible app (initial stack not in SRAM — e.g. erased flash):
    // drop to BOOTSEL so the module is recoverable over USB.
    if ((app_vectors[0] >> 28) != 0x2u)
        reset_usb_boot(0, 0);

    // Quiesce anything our runtime enabled before the app's crt0 takes over.
    nvic_hw->icer = 0xFFFFFFFFu;
    nvic_hw->icpr = 0xFFFFFFFFu;
    scb_hw->vtor = (uint32_t)(uintptr_t)app_vectors;
    __asm volatile(
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r"(app_vectors[0]), "r"(app_vectors[1])
        :);
    __builtin_unreachable();
}
