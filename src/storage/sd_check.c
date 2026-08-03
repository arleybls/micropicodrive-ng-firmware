// "SD Check" config-menu mode - mounts the SD card, reports card/filesystem
// info, and runs a timed write/read/verify pass. Results on the display
// (band screens, same style as the OTA modes) and mirrored to serial.
//
// This is the sandbox testbed for the full-FatFs migration planned for
// MicroPicoDrive (whose Petit FatFs cannot create files) - see
// PLAN_MICRODRIVE_REUSE.md and the standing sync rule for its driver plan.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "f_util.h"
#include "UserInterfaceExtension.h"
#include "sd_check.h"
#include "sd_menu.h"   // shared mount (sd_fs_mount) - do not unmount here

#define SD_TEST_FILE   "UIEXTTST.BIN"
#define SD_TEST_CHUNK  4096
#define SD_TEST_CHUNKS 8   // 32 KB total - quick but enough for a real rate

static uint8_t s_buf[SD_TEST_CHUNK];

static void wait_button(void) {
    // wait for release of anything held, then for a SELECT/K4 press
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(150);
    while (gpio_get(UIEXT_BTN_SELECT) != 0 && gpio_get(UIEXT_BTN_CONFIG) != 0)
        sleep_ms(20);
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(150);
}

static void fail_screen(const char *step, FRESULT fr) {
    // On screen only the numeric code: FRESULT_str() sentences ("A hard
    // error occurred in ...") overflow the display and char-truncation
    // lost the code itself. The full sentence still goes to serial.
    char l2[26];
    snprintf(l2, sizeof(l2), "Error %d", (int)fr);
    printf("[sd] %s failed: %s (%d)\n", step, FRESULT_str(fr), (int)fr);
    uiext_ota_status2("SD Check", step, l2);
    wait_button();
}

// bytes -> "X.Y" GB parts (binary GiB units, one decimal)
static void gb_parts(uint64_t bytes, uint32_t *whole, uint32_t *tenth) {
    uint32_t mb = (uint32_t)(bytes >> 20);
    *whole = mb / 1024;
    *tenth = (mb % 1024) * 10 / 1024;
}

void sd_check_run(void) {
    FIL f;
    FRESULT fr;
    UINT n;

    uiext_ota_screen("SD Check", "Mounting...", "", "");
    printf("[sd] mounting...\n");
    fr = sd_fs_mount();
    if (fr != FR_OK) { fail_screen("Mount failed", fr); return; }

    // -- Card / filesystem info ---------------------------------------------
    DWORD free_clst = 0;
    FATFS *pfs = NULL;
    fr = f_getfree("", &free_clst, &pfs);
    if (fr != FR_OK || !pfs) { fail_screen("Getfree failed", fr); return; }
    const char *fstype = pfs->fs_type == FS_FAT12 ? "FAT12" :
                         pfs->fs_type == FS_FAT16 ? "FAT16" :
                         pfs->fs_type == FS_FAT32 ? "FAT32" :
                         pfs->fs_type == FS_EXFAT ? "exFAT" : "FAT?";
    uint32_t tw, tt, fw, ft;
    gb_parts((uint64_t)(pfs->n_fatent - 2) * pfs->csize * 512, &tw, &tt);
    gb_parts((uint64_t)free_clst * pfs->csize * 512, &fw, &ft);
    char l_total[26], l_free[26];
    snprintf(l_total, sizeof(l_total), "%s %lu.%lu GB",
             fstype, (unsigned long)tw, (unsigned long)tt);
    snprintf(l_free, sizeof(l_free), "Free: %lu.%lu GB",
             (unsigned long)fw, (unsigned long)ft);
    printf("[sd] %s | %s\n", l_total, l_free);
    uiext_ota_screen("SD Check", l_total, l_free, "Writing...");

    // -- Timed write --------------------------------------------------------
    for (unsigned i = 0; i < sizeof(s_buf); i++) s_buf[i] = (uint8_t)(i * 7 + 13);
    fr = f_open(&f, SD_TEST_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { fail_screen("Create failed", fr); return; }
    uint64_t t0 = time_us_64();
    for (int c = 0; c < SD_TEST_CHUNKS; c++) {
        fr = f_write(&f, s_buf, sizeof(s_buf), &n);
        if (fr != FR_OK || n != sizeof(s_buf)) {
            f_close(&f);
            fail_screen("Write failed", fr);
            return;
        }
    }
    f_close(&f);   // includes flush - count it in the write time
    uint32_t wr_kbs = (uint32_t)((uint64_t)SD_TEST_CHUNKS * SD_TEST_CHUNK * 1000000
                                 / (time_us_64() - t0) / 1024);
    char wline[26];
    snprintf(wline, sizeof(wline), "Write OK  %lu kB/s", (unsigned long)wr_kbs);
    printf("[sd] %s\n", wline);
    uiext_ota_screen("SD Check", l_total, wline, "Reading...");

    // -- Timed read + verify ------------------------------------------------
    fr = f_open(&f, SD_TEST_FILE, FA_READ);
    if (fr != FR_OK) { fail_screen("Reopen failed", fr); return; }
    t0 = time_us_64();
    bool verify_ok = true;
    for (int c = 0; c < SD_TEST_CHUNKS; c++) {
        fr = f_read(&f, s_buf, sizeof(s_buf), &n);
        if (fr != FR_OK || n != sizeof(s_buf)) {
            f_close(&f);
            fail_screen("Read failed", fr);
            return;
        }
        for (unsigned i = 0; i < sizeof(s_buf); i += 97)
            if (s_buf[i] != (uint8_t)(i * 7 + 13)) { verify_ok = false; break; }
    }
    uint32_t rd_kbs = (uint32_t)((uint64_t)SD_TEST_CHUNKS * SD_TEST_CHUNK * 1000000
                                 / (time_us_64() - t0) / 1024);
    f_close(&f);
    f_unlink(SD_TEST_FILE);

    // A rate measured over corrupted data is meaningless, and the combined
    // "VERIFY FAIL  NNNN kB/s" line overflowed the band anyway.
    char rline[26];
    if (verify_ok)
        snprintf(rline, sizeof(rline), "Read OK  %lu kB/s", (unsigned long)rd_kbs);
    else
        snprintf(rline, sizeof(rline), "VERIFY FAIL");
    printf("[sd] %s\n", rline);

    const char *report[] = { l_total, l_free, wline, rline };
    uiext_report_view("SD Check", report, (int)(sizeof(report) / sizeof(report[0])));
}
