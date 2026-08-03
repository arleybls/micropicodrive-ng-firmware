// SD-card firmware update (boot-time, from /.update). See sd_update.h.
//
// Lite (RP2040 flashloader) flavor of the mainline A/B feature: the UF2 is
// validated and CRC'd in a scan pass, staged into the staging flash region
// with the header page written LAST (a valid header means "complete"), then
// the module reboots and the flashloader copies the image over the app.
// Interrupted staging leaves the header invalid — the old firmware just
// boots; interrupted apply is retried by the flashloader every boot.
//
// Called with the display up and no cartridge loaded (INIT_SCREEN), so the
// flash_safe_execute writes are safe — same gate as the settings sector.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/regs/addressmap.h"
#include "pico/binary_info/defs.h"
#include "pico/binary_info/structure.h"
#include "ff.h"
#include "sd_menu.h"
#include "UserInterface.h"   //cfInserted/mdInUse — flash-write gate
#include "UserInterfaceExtension.h"
#include "flash_layout.h"
#include "sd_update.h"

#define UPD_DIR "/.update"

// UF2 block layout (512 bytes) — https://github.com/microsoft/uf2
#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u
#define UF2_FLAG_FAMILY  0x00002000u
#define UF2_FAMILY_RP2040 0xE48BFF56u

typedef struct {
    uint32_t magic_start0, magic_start1, flags, target_addr;
    uint32_t payload_size, block_no, num_blocks, family_id;
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;

static uf2_block_t s_block;
static uint8_t     s_sector[FLASH_SECTOR_SIZE];

static void msg(const char *l1, const char *l2, uint32_t ms) {
    if (l2 && l2[0]) uiext_ota_status2("SD Update", l1, l2);
    else             uiext_ota_status("SD Update", l1);
    printf("[upd] %s %s\n", l1, l2 ? l2 : "");
    if (ms) sleep_ms(ms);
}

// flash_safe_execute wrappers (multicore lockout — core 0 opted in at boot).
// Return 0 on success, mirroring "if (op()) fail" call sites.
typedef struct { uint32_t off; const uint8_t *data; uint32_t len; } flash_op_t;

static void cb_erase(void *param) {
    const flash_op_t *op = (const flash_op_t *)param;
    flash_range_erase(op->off, op->len);
}

static void cb_program(void *param) {
    const flash_op_t *op = (const flash_op_t *)param;
    flash_range_program(op->off, op->data, op->len);
}

static int flash_safe_execute_erase(uint32_t off, uint32_t len) {
    flash_op_t op = { off, NULL, len };
    return flash_safe_execute(cb_erase, &op, 100) != PICO_OK;
}

static int flash_safe_execute_program(uint32_t off, const uint8_t *data, uint32_t len) {
    flash_op_t op = { off, data, len };
    return flash_safe_execute(cb_program, &op, 100) != PICO_OK;
}

// One scan/stage pass over the UF2. Blocks aimed at the flashloader region
// are ignored (so both the app-only and the merged "full" UF2 work; the
// flashloader is deliberately not field-updatable). App-range blocks must be
// 256-byte payloads, contiguous from FL_APP_OFFSET.
// Returns the image size in bytes (0 on error, message already shown) and
// the running CRC in *crc. When `stage` is set, payloads are streamed into
// the staging region (which must already be erased).
static uint32_t uf2_pass(FIL *f, uint32_t *crc, bool stage, uint32_t total_size) {
    uint32_t size = 0;
    uint32_t sect_fill = 0;
    uint32_t last_bno = 0, last_nblk = 0;
    int last_pct = -1;
    *crc = 0xFFFFFFFFu;

    for (;;) {
        UINT n = 0;
        if (f_read(f, &s_block, sizeof(s_block), &n) != FR_OK) {
            msg("Read failed", "update aborted", 2500);
            return 0;
        }
        if (n == 0) break;                       // EOF
        if (n != sizeof(s_block) ||
            s_block.magic_start0 != UF2_MAGIC_START0 ||
            s_block.magic_start1 != UF2_MAGIC_START1 ||
            s_block.magic_end != UF2_MAGIC_END) {
            msg("Not a UF2 file", "", 2500);
            return 0;
        }
        if (!(s_block.flags & UF2_FLAG_FAMILY) ||
            s_block.family_id != UF2_FAMILY_RP2040) {
            msg("Wrong board family", "(not RP2040)", 2500);
            return 0;
        }

        // Only app-range blocks are staged. Anything else inside flash —
        // flashloader, the merged image's stage_clear block, settings — is
        // ignored, so the full BOOTSEL UF2 also works as an SD drop.
        if (s_block.target_addr < XIP_BASE + FL_APP_OFFSET ||
            s_block.target_addr >= XIP_BASE + FL_APP_OFFSET + FL_APP_MAX_SIZE) {
            if (s_block.target_addr >= XIP_BASE &&
                s_block.target_addr < XIP_BASE + PICO_FLASH_SIZE_BYTES)
                continue;
            msg("Bad UF2 layout", "", 2500);
            return 0;
        }

        if (s_block.payload_size != 256u ||
            s_block.target_addr != XIP_BASE + FL_APP_OFFSET + size ||
            size + 256u > FL_APP_MAX_SIZE || size + 256u > FL_STAGE_MAX_SIZE) {
            msg("Bad UF2 layout", "", 2500);
            return 0;
        }

        // Truncation defense: a file cut at a block boundary would otherwise
        // self-verify (the CRC is computed from what we read). Require the
        // app blocks' own numbering to be contiguous and, at EOF below, to
        // have reached num_blocks - 1.
        if (size != 0 && s_block.block_no != last_bno + 1) {
            msg("Bad UF2 layout", "", 2500);
            return 0;
        }
        last_bno  = s_block.block_no;
        last_nblk = s_block.num_blocks;

        *crc = fl_crc32_update(*crc, s_block.data, 256u);
        size += 256u;

        if (stage) {
            memcpy(&s_sector[sect_fill], s_block.data, 256u);
            sect_fill += 256u;
            if (sect_fill == FLASH_SECTOR_SIZE) {
                if (flash_safe_execute_program(FL_STAGE_IMG_OFFSET + size - sect_fill,
                                               s_sector, FLASH_SECTOR_SIZE))
                    return 0;
                sect_fill = 0;
            }
            // Pre-commit staging is inert (header written only after the
            // user confirms), so no power-off warning here. Progress uses
            // the mainline Receiving/Flashing chrome (big counter in its
            // own window); the title stays "Reading" because this runs
            // BEFORE the confirm — nothing is being applied yet.
            int pct = (int)((uint64_t)size * 100 / total_size);
            if (pct != last_pct) {
                last_pct = pct;
                uiext_ota_progress_update(pct);
            }
        }
    }

    if (stage && sect_fill) {                    // final partial sector
        memset(&s_sector[sect_fill], 0xFF, FLASH_SECTOR_SIZE - sect_fill);
        if (flash_safe_execute_program(FL_STAGE_IMG_OFFSET + size - sect_fill,
                                       s_sector, FLASH_SECTOR_SIZE))
            return 0;
    }

    if (size < 256u) {
        msg("Empty image", "", 2500);
        return 0;
    }
    if (last_bno != last_nblk - 1) {
        msg("Truncated file", "", 2500);
        return 0;
    }
    *crc = ~*crc;
    return size;
}

// Translate a runtime app address into the staged copy of the image.
static const void *staged_ptr(uint32_t addr, uint32_t size) {
    if (addr < XIP_BASE + FL_APP_OFFSET || addr >= XIP_BASE + FL_APP_OFFSET + size)
        return NULL;
    return (const void *)(XIP_BASE + FL_STAGE_IMG_OFFSET + (addr - XIP_BASE - FL_APP_OFFSET));
}

// The pico_set_program_version string embedded in the staged image's binary
// info (what picotool shows as "version"). NULL when absent.
static const char *staged_version_string(uint32_t size) {
    const uint32_t *img = (const uint32_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET);
    uint32_t search_words = (size < 1024u ? size : 1024u) / 4u;
    // Marker block sits early in .text, just after the vector table.
    for (uint32_t i = 0; i + 4 < search_words; i++) {
        if (img[i] != BINARY_INFO_MARKER_START || img[i + 4] != BINARY_INFO_MARKER_END)
            continue;
        const uint32_t *entry     = staged_ptr(img[i + 1], size);
        const uint32_t *entry_end = staged_ptr(img[i + 2], size);
        if (!entry || !entry_end) return NULL;
        for (; entry < entry_end; entry++) {
            const binary_info_id_and_string_t *bi = staged_ptr(*entry, size);
            if (bi && bi->core.type == BINARY_INFO_TYPE_ID_AND_STRING &&
                bi->core.tag == BINARY_INFO_TAG_RASPBERRY_PI &&
                bi->id == BINARY_INFO_ID_RP_PROGRAM_VERSION_STRING)
                return (const char *)staged_ptr((uint32_t)(uintptr_t)bi->value, size);
        }
        return NULL;
    }
    return NULL;
}

void sd_update_check_at_boot(void) {
    // Hard rule (same as the settings save): no flash writes with a
    // cartridge mounted or the drive in use. Power-on always arrives here
    // ejected, but the UI-reconnect path (CARTRIDGE_READY -> IDLE ->
    // INIT_SCREEN on a PIN_UI_DETECT glitch) re-enters WITH the cartridge
    // loaded — a lockout or reboot then would corrupt the live QL session.
    if (cfInserted != NONE || mdInUse)
        return;

    // Staging hygiene: the flashloader applies a valid, differing staged
    // image BEFORE this app can run — so observing that state here means
    // the app was reflashed externally (BOOTSEL rollback) or the apply is
    // failing. Either way the staged image is stale: invalidate it, or the
    // flashloader would re-apply it over the rollback on the next boot.
    const fl_stage_hdr_t *shdr = (const fl_stage_hdr_t *)(XIP_BASE + FL_STAGE_HDR_OFFSET);
    if (shdr->magic == FL_STAGE_MAGIC &&
        shdr->size >= 256u && shdr->size <= FL_STAGE_MAX_SIZE && shdr->size <= FL_APP_MAX_SIZE &&
        fl_crc32((const uint8_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET), shdr->size) == shdr->crc32 &&
        fl_crc32((const uint8_t *)(XIP_BASE + FL_APP_OFFSET), shdr->size) != shdr->crc32) {
        printf("[upd] stale staged image (external reflash?) - invalidating\n");
        flash_safe_execute_erase(FL_STAGE_HDR_OFFSET, FLASH_SECTOR_SIZE);
    }

    if (sd_fs_mount() != FR_OK) return;

    // First *.uf2 in /.update
    DIR dir;
    FILINFO fi;
    char path[96] = {0};
    if (f_opendir(&dir, UPD_DIR) != FR_OK) return;   // no .update folder
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t l = strlen(fi.fname);
        if (!(fi.fattrib & AM_DIR) &&
            l > 4 && strcasecmp(fi.fname + l - 4, ".uf2") == 0 &&
            l <= sizeof(path) - sizeof(UPD_DIR) - 2) {
            snprintf(path, sizeof(path), UPD_DIR "/%s", fi.fname);
            break;
        }
    }
    f_closedir(&dir);
    if (!path[0]) return;
    printf("[upd] found %s\n", path);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) { msg("Open failed", "", 2000); return; }

    // Scan pass: validate + size + CRC, no writes. Takes a second or two —
    // say so instead of leaving the fresh display blank.
    msg("Checking update", "", 0);
    uint32_t crc = 0;
    uint32_t size = uf2_pass(&f, &crc, false, 0);
    if (!size) { f_close(&f); return; }

    // Identical to the running app (typically right after an update, with
    // the file still on the card): nothing to do.
    if (crc == fl_crc32((const uint8_t *)(XIP_BASE + FL_APP_OFFSET), size)) {
        f_close(&f);
        printf("[upd] image matches running app, skipping\n");
        return;
    }

    // Stage BEFORE asking: the staged image is inert until the header page
    // is committed, and having it in flash lets the confirm screen show the
    // update's embedded version instead of a clipped filename. If a prior
    // boot already staged this exact image (timed-out prompt, file left on
    // card), reuse it — no per-boot staging wear, and the prompt appears
    // near-instantly. Reuse also requires the header sector to be erased
    // (partial header writes must go through a fresh erase).
    bool already_staged =
        fl_crc32((const uint8_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET), size) == crc &&
        shdr->magic == 0xFFFFFFFFu && shdr->size == 0xFFFFFFFFu &&
        shdr->crc32 == 0xFFFFFFFFu;

    if (already_staged) {
        f_close(&f);
        printf("[upd] image already staged, reusing\n");
    } else {
        // Erase the header sector FIRST (a stale "complete" mark must never
        // describe a half-staged image), then one sector per lockout so
        // core 0's blackout stays bounded at a single sector erase.
        uint32_t img_sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
        uiext_ota_progress("Reading", "Firmware update", 0);
        for (uint32_t s = 0; s <= img_sectors; s++) {
            if (flash_safe_execute_erase(FL_STAGE_HDR_OFFSET + s * FLASH_SECTOR_SIZE,
                                         FLASH_SECTOR_SIZE)) {
                f_close(&f);
                msg("Erase failed", "update aborted", 2500);
                return;
            }
        }

        // Stage pass: stream payloads into the staging region.
        if (f_lseek(&f, 0) != FR_OK) { f_close(&f); msg("Read failed", "update aborted", 2500); return; }
        uint32_t crc2 = 0;
        uint32_t size2 = uf2_pass(&f, &crc2, true, size);
        f_close(&f);
        if (!size2) { msg("Stage failed", "update aborted", 2500); return; }

        // Verify what actually landed in flash before offering it.
        if (size2 != size || crc2 != crc ||
            fl_crc32((const uint8_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET), size) != crc) {
            msg("Verify failed", "update aborted", 2500);
            return;
        }
    }

    // Sanity: a real app image has its vector table (initial SP in SRAM) at
    // +0x100. Rejects e.g. a pre-flashloader flat UF2, whose app-range
    // blocks are just the tail of a flat image.
    uint32_t staged_msp = *(const uint32_t *)(XIP_BASE + FL_STAGE_IMG_OFFSET + 0x100u);
    if ((staged_msp >> 28) != 0x2u) {
        msg("Bad app image", "update aborted", 2500);
        return;
    }

    // Ask the user (default No, 60 s timeout): running version vs the update
    // file's embedded version, "(older)" marking a downgrade. An explicit No
    // deletes the update file — declining sticks and the boot prompt does not
    // return (UX review X3). A timeout means nobody answered: the file (and
    // the inert staged image) is kept and offered again next boot (X4).
    const char *newver = staged_version_string(size);
    char cur[26], neu[26];
    snprintf(cur, sizeof(cur), "Current: v%d.%d.%d",
             UIEXT_FW_VERSION_MAJOR, UIEXT_FW_VERSION_MINOR, UIEXT_FW_VERSION_PATCH);
    unsigned nmaj, nmin, npat;
    bool older = false;
    if (newver && sscanf(newver, "%u.%u.%u", &nmaj, &nmin, &npat) == 3) {
        uint32_t cur_key = ((uint32_t)UIEXT_FW_VERSION_MAJOR << 20) |
                           ((uint32_t)UIEXT_FW_VERSION_MINOR << 10) |
                            (uint32_t)UIEXT_FW_VERSION_PATCH;
        uint32_t new_key = (nmaj << 20) | (nmin << 10) | npat;
        older = new_key < cur_key;
    }
    if (newver) snprintf(neu, sizeof(neu), "SD: v%.12s%s", newver, older ? " (older)" : "");
    else        snprintf(neu, sizeof(neu), "SD: unknown");

    uiext_confirm_res_t ans = uiext_ota_confirm2("Firmware Update", cur, neu,
                                                 UIEXT_CONFIRM_TIMEOUT_MS);
    if (ans == UIEXT_CONFIRM_NO) {
        if (f_unlink(path) != FR_OK)
            printf("[upd] could not remove %s\n", path);
        msg("Update declined", "file removed", 2000);
        return;
    }
    if (ans != UIEXT_CONFIRM_YES)
        return;   // timeout: keep the file, ask again next boot

    memset(s_sector, 0xFF, FLASH_PAGE_SIZE);
    fl_stage_hdr_t hdr = { FL_STAGE_MAGIC, size, crc, 0xFFFFFFFFu };
    memcpy(s_sector, &hdr, sizeof(hdr));
    if (flash_safe_execute_program(FL_STAGE_HDR_OFFSET, s_sector, FLASH_PAGE_SIZE)) {
        msg("Commit failed", "", 2500);
        return;
    }

    // Consumed: the committed staged image is now the source of truth (an
    // interrupted apply retries from staging, never from the file), so the
    // file only costs a full re-scan on every later boot. Failure (e.g.
    // write-protected card) is fine — the skip-if-same check covers it.
    if (f_unlink(path) != FR_OK)
        printf("[upd] could not remove %s\n", path);

    // Same chrome + braille spinner as the boot screen while going down.
    printf("[upd] staged ok, rebooting\n");
    uiext_ota_wait("Firmware Update", "Restarting..");
    for (int frame = 0; frame < 20; frame++) {
        uiext_boot_spinner(frame);
        sleep_ms(80);
    }
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}
