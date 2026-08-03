// BTstack bond-storage flash bank — corrected for A/B partition boots.
//
// Vendored from pico-sdk 2.2.0 src/rp2_common/pico_btstack/btstack_flash_bank.c
// (BSD-3-Clause, Raspberry Pi Ltd) with two fixes for partition boots:
//  1. Reads go through XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE (physical flash
//     view). The original reads via XIP_BASE, which under QMI address
//     translation only maps the booted partition — reading the default
//     end-of-flash offset stalls the bus forever (the Pair Device freeze).
//  2. Storage lives at a fixed physical offset inside our "data" partition
//     (see partition_table.json) so bonds survive A/B swaps and never
//     collide with firmware slots.
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hal_flash_bank.h"
#include <string.h>
#include <assert.h>

// Start of the "data" partition — from `picotool partition info` on the real
// device (picotool places fw_a at 8K, not 4K): fw_a 0x002000, fw_b 0x182000,
// cyw43-fw 0x302000, data 0x342000. Keep in sync with partition_table.json.
// NOTE: the sector AFTER the two banks (0x344000) holds the display
// settings (UserInterface.c SETTINGS_FLASH_OFFSET) — keep them in step.
#define UIEXT_TLV_STORAGE_OFFSET 0x342000u
#define UIEXT_BANK_SIZE          FLASH_SECTOR_SIZE   // 4K per bank, 2 banks

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static uint32_t bank_get_size(void *context) {
    (void)context;
    return UIEXT_BANK_SIZE;
}

static uint32_t bank_get_alignment(void *context) {
    (void)context;
    return 1;
}

typedef struct {
    bool op_is_erase;
    uintptr_t p0;
    uintptr_t p1;
} mutation_operation_t;

static void bank_mutate(void *param) {
    const mutation_operation_t *mop = (const mutation_operation_t *)param;
    if (mop->op_is_erase) {
        flash_range_erase(mop->p0, UIEXT_BANK_SIZE);
    } else {
        flash_range_program(mop->p0, (const uint8_t *)mop->p1, FLASH_PAGE_SIZE);
    }
}

static inline const uint8_t *bank_phys_ptr(uint32_t flash_offset) {
    // Physical (untranslated) flash view — valid regardless of which A/B
    // partition we booted from.
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + flash_offset);
}

static void bank_erase(void *context, int bank) {
    (void)context;
    mutation_operation_t mop = {
        .op_is_erase = true,
        .p0 = UIEXT_TLV_STORAGE_OFFSET + (UIEXT_BANK_SIZE * (uint32_t)bank),
    };
    flash_safe_execute(bank_mutate, &mop, UINT32_MAX);
}

static void bank_read(void *context, int bank, uint32_t offset, uint8_t *buffer, uint32_t size) {
    (void)context;
    assert(bank <= 1 && offset < UIEXT_BANK_SIZE && offset + size <= UIEXT_BANK_SIZE);
    if (bank > 1 || offset >= UIEXT_BANK_SIZE || offset + size > UIEXT_BANK_SIZE) return;
    memcpy(buffer,
           bank_phys_ptr(UIEXT_TLV_STORAGE_OFFSET + (UIEXT_BANK_SIZE * (uint32_t)bank) + offset),
           size);
}

static void bank_write(void *context, int bank, uint32_t offset, const uint8_t *data, uint32_t size) {
    (void)context;
    assert(bank <= 1 && offset < UIEXT_BANK_SIZE && offset + size <= UIEXT_BANK_SIZE);
    if (bank > 1 || offset >= UIEXT_BANK_SIZE || offset + size > UIEXT_BANK_SIZE) return;
    if (size == 0) return;

    const uint32_t bank_start = UIEXT_TLV_STORAGE_OFFSET + (UIEXT_BANK_SIZE * (uint32_t)bank);
    const uint32_t first_page = offset / FLASH_PAGE_SIZE;
    const uint32_t last_page  = (offset + size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    offset %= FLASH_PAGE_SIZE;

    uint32_t data_pos = 0;
    uint32_t size_left = size;
    for (uint32_t page = first_page; page < last_page; page++) {
        uint8_t page_data[FLASH_PAGE_SIZE];
        if (page == first_page && offset > 0) {
            memcpy(page_data, bank_phys_ptr(bank_start + page * FLASH_PAGE_SIZE), offset);
        }
        if (page == last_page - 1 && (offset + size_left) < FLASH_PAGE_SIZE) {
            memcpy(page_data + offset + size_left,
                   bank_phys_ptr(bank_start + page * FLASH_PAGE_SIZE + offset + size_left),
                   FLASH_PAGE_SIZE - offset - size_left);
        }
        const uint32_t size_to_copy = MIN(size_left, FLASH_PAGE_SIZE - offset);
        memcpy(page_data + offset, data + data_pos, size_to_copy);
        data_pos += size_to_copy;
        size_left -= size_to_copy;
        offset = 0;

        mutation_operation_t mop = {
            .op_is_erase = false,
            .p0 = bank_start + page * FLASH_PAGE_SIZE,
            .p1 = (uintptr_t)page_data,
        };
        flash_safe_execute(bank_mutate, &mop, UINT32_MAX);
    }
}

static const hal_flash_bank_t uiext_flash_bank_obj = {
    .get_size      = &bank_get_size,
    .get_alignment = &bank_get_alignment,
    .erase         = &bank_erase,
    .read          = &bank_read,
    .write         = &bank_write,
};

const hal_flash_bank_t *uiext_flash_bank_instance(void) {
    return &uiext_flash_bank_obj;
}
