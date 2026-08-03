// SD-card firmware update (boot-time, from /.update). See sd_update.h.
//
// Reuses the OTA safety model: image goes to the INACTIVE slot with the
// header sector held back until fully verified (SHA-256 vs the manifest),
// so an interrupted or corrupt update never produces a bootable half-image.
// Downgrades are allowed (physical card access = authorization): after
// flashing an older version, the ACTIVE slot's header is invalidated so the
// bootrom picks the older image (same mechanism as Revert FW).
#include "UserInterfaceExtension.h"
// No #if here on purpose: CMake compiles either this file or sd_update_lite.c,
// never both. A self-gate would turn a mis-selected source list into a silent
// no-op instead of a loud link error — the failure mode this fold exists to
// remove (study traps T1/T2/T7).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/sha256.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "f_util.h"
#include "sd_menu.h"
#include "sd_update.h"
#include "UserInterface.h"   //cfInserted/mdInUse — flash-write gate
#include "ota_ble.h"

#define UPD_DIR "/.update"

static uint8_t s_chunk[4096];
static uint8_t s_sector0[4096];

static bool hex_to_bytes(const char *hex, uint8_t *out, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

// Parse the build_ota.ps1 manifest:
// {"major":M,"minor":m,"patch":p,"size":S,"sha256":"..."} — patch is
// optional (0 for pre-2.5.1 manifests).
static bool parse_manifest(const char *path, uint16_t *maj, uint16_t *min,
                           uint16_t *pat, uint32_t *size, uint8_t sha[32]) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    char txt[512];
    UINT n = 0;
    FRESULT fr = f_read(&f, txt, sizeof(txt) - 1, &n);
    f_close(&f);
    if (fr != FR_OK || n == 0) return false;
    txt[n] = '\0';

    const char *p;
    if (!(p = strstr(txt, "\"major\"")) || !(p = strchr(p, ':'))) return false;
    *maj = (uint16_t)strtoul(p + 1, NULL, 10);
    if (!(p = strstr(txt, "\"minor\"")) || !(p = strchr(p, ':'))) return false;
    *min = (uint16_t)strtoul(p + 1, NULL, 10);
    *pat = 0;
    if ((p = strstr(txt, "\"patch\"")) && (p = strchr(p, ':')))
        *pat = (uint16_t)strtoul(p + 1, NULL, 10);
    if (!(p = strstr(txt, "\"size\"")) || !(p = strchr(p, ':'))) return false;
    *size = (uint32_t)strtoul(p + 1, NULL, 10);
    if (!(p = strstr(txt, "\"sha256\"")) || !(p = strchr(p, ':')) || !(p = strchr(p, '"')))
        return false;
    return hex_to_bytes(p + 1, sha, 32);
}

static void delete_pair(const char *bin_path, const char *json_path) {
    f_unlink(bin_path);
    f_unlink(json_path);
    printf("[upd] removed %s + manifest\n", bin_path);
}

static void msg(const char *l1, const char *l2, uint32_t ms) {
    if (l2 && l2[0]) uiext_ota_status2("SD Update", l1, l2);
    else             uiext_ota_status("SD Update", l1);
    printf("[upd] %s %s\n", l1, l2 ? l2 : "");
    if (ms) sleep_ms(ms);
}

void sd_update_check_at_boot(void) {
    // Hard rule (same as the radio modes): no flash writes with a cartridge
    // mounted or the drive in use. Power-on always arrives here ejected,
    // but the UI-reconnect path (CARTRIDGE_READY -> IDLE -> INIT_SCREEN on
    // a PIN_UI_DETECT glitch) re-enters WITH the cartridge loaded — slot
    // flashing and the reboot would corrupt the live QL session.
    if (cfInserted != NONE || mdInUse)
        return;

    if (sd_fs_mount() != FR_OK) return;

    // Find the first *.bin with a matching *.json in /.update
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, UPD_DIR) != FR_OK) return;   // no .update folder
    char bin_path[96] = {0}, json_path[96] = {0};
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t l = strlen(fi.fname);
        if (l > 4 && strcasecmp(fi.fname + l - 4, ".bin") == 0 &&
            l - 4 <= sizeof(bin_path) - sizeof(UPD_DIR) - 7) {
            snprintf(bin_path, sizeof(bin_path), UPD_DIR "/%s", fi.fname);
            snprintf(json_path, sizeof(json_path), UPD_DIR "/%.*s.json",
                     (int)(l - 4), fi.fname);
            FILINFO jf;
            if (f_stat(json_path, &jf) == FR_OK) break;   // found a pair
            bin_path[0] = '\0';
        }
    }
    f_closedir(&dir);
    if (!bin_path[0]) return;
    printf("[upd] found %s\n", bin_path);

    uint16_t maj, min, pat;
    uint32_t size;
    uint8_t expect_sha[32];
    if (!parse_manifest(json_path, &maj, &min, &pat, &size, expect_sha)) {
        msg("Bad manifest", "file removed", 2000);
        delete_pair(bin_path, json_path);
        return;
    }

    // Combined minor*100+patch ordering (see ota_ble.h).
    uint32_t ver_new = ((uint32_t)maj << 16) | (min * 100u + pat);
    uint32_t ver_cur = ((uint32_t)UIEXT_FW_VERSION_MAJOR << 16) | UIEXT_FW_VERNUM_MINOR;
    if (ver_new == ver_cur) return;   // same version: ignore, leave file alone

    uint32_t act_off, tgt_off, tgt_size;
    if (!ota_slots(&act_off, &tgt_off, &tgt_size)) {
        msg("No partition table", "cannot update", 2000);
        return;
    }
    if (size < sizeof(s_sector0) || size > tgt_size) {
        msg("Bad image size", "file removed", 2000);
        delete_pair(bin_path, json_path);
        return;
    }

    // Ask the user (default No, shared 30 s timeout). "SD:" not "SD card:" —
    // the worst case "SD card: v2.5.1 (older)" overflowed the band and clipped
    // exactly the "(older)" warning (UX review R6). An explicit No deletes
    // the update pair — declining sticks (X3 parity with Lite); a timeout
    // means nobody answered, so the files are kept and offered again next
    // boot (X4: absence must never destroy files).
    char cur[26], neu[26];
    snprintf(cur, sizeof(cur), "Current: %s", ota_fw_version_string());
    snprintf(neu, sizeof(neu), "SD: v%u.%u.%u%s", maj, min, pat,
             ver_new < ver_cur ? " (older)" : "");
    uiext_confirm_res_t ans = uiext_ota_confirm2("Firmware Update", cur, neu,
                                                 UIEXT_CONFIRM_TIMEOUT_MS);
    if (ans == UIEXT_CONFIRM_NO) {
        delete_pair(bin_path, json_path);
        msg("Update declined", "files removed", 2000);
        return;
    }
    if (ans != UIEXT_CONFIRM_YES)
        return;   // timeout: keep the files, ask again next boot

    // Flash: sector 0 held back until the hash verifies
    FIL f;
    if (f_open(&f, bin_path, FA_READ) != FR_OK) { msg("Open failed", "", 2000); return; }
    if (f_size(&f) != size) {
        f_close(&f);
        msg("Size mismatch", "file removed", 2000);
        delete_pair(bin_path, json_path);
        return;
    }

    pico_sha256_state_t sha;
    if (pico_sha256_start_blocking(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK) {
        f_close(&f);
        msg("SHA engine busy", "", 2000);
        return;
    }

    // Same chrome as the BLE Receiving screen: centered title + version
    // line + the big percentage counter, flicker-free (counter window
    // only after the first draw).
    char fwline[24];
    snprintf(fwline, sizeof(fwline), "Firmware v%u.%u.%u", maj, min, pat);
    uiext_ota_progress("Flashing", fwline, 0);

    bool ok = true;
    UINT n = 0;
    ok = f_read(&f, s_sector0, sizeof(s_sector0), &n) == FR_OK && n == sizeof(s_sector0);
    if (ok) pico_sha256_update(&sha, s_sector0, sizeof(s_sector0));
    uint32_t off = sizeof(s_sector0);
    int last_pct = -1;
    while (ok && off < size) {
        UINT want = size - off < sizeof(s_chunk) ? (UINT)(size - off) : sizeof(s_chunk);
        ok = f_read(&f, s_chunk, want, &n) == FR_OK && n == want;
        if (!ok) break;
        pico_sha256_update(&sha, s_chunk, n);
        ok = ota_flash_sector(tgt_off + off, s_chunk, n);
        off += n;
        int pct = (int)((uint64_t)off * 100 / size);
        if (pct != last_pct) {
            last_pct = pct;
            uiext_ota_progress_update(pct);
        }
    }
    f_close(&f);

    sha256_result_t res;
    pico_sha256_finish(&sha, &res);
    if (!ok) {
        ota_erase_sector_at(tgt_off);   // ensure nothing bootable was left
        msg("Flash/read error", "update aborted", 2500);
        return;                          // keep files for a retry
    }
    if (memcmp(res.bytes, expect_sha, 32) != 0) {
        ota_erase_sector_at(tgt_off);
        msg("Hash mismatch", "file removed", 2500);
        delete_pair(bin_path, json_path);
        return;
    }

    // Commit: write the held-back header; for downgrades also invalidate the
    // active slot so the bootrom picks the older image.
    if (!ota_flash_sector(tgt_off, s_sector0, sizeof(s_sector0))) {
        msg("Commit failed", "", 2500);
        return;
    }
    if (ver_new < ver_cur) ota_erase_sector_at(act_off);

    delete_pair(bin_path, json_path);   // before reboot: never re-trigger
    msg("Updated OK", "restarting...", 800);
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}
