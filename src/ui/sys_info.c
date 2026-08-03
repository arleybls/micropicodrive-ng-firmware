// "System Info" config-menu mode. Reports RP2350 chip/clock/temperature, RAM
// (heap) and flash usage, firmware version and the unique board id, on the same
// scrollable band screen the SD Check mode uses. Read-only; no radio, no SD.
#include <stdio.h>
#include <malloc.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "UserInterfaceExtension.h"
#include "sys_info.h"
#if UIEXT_OTA_ENABLED
#include "ota_ble.h"   // ota_fw_version_string()
#endif

// Linker-provided extents (Pico SDK): RAM top, heap region, and the flash image.
extern char __StackTop, __StackLimit, __bss_end__;
extern char __flash_binary_start, __flash_binary_end;

#define UIEXT_SRAM_BASE 0x20000000u   // RP2350 SRAM base

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)   // Pico 2 W default
#endif

// On-chip temperature sensor (ADC channel 4), averaged; RP2040/RP2350 formula.
static float read_temp_c(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += adc_read();
    float v = (acc / 8.0f) * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

void sys_info_run(void) {
    char fw[26], cpu[26], clk[26], temp[26], ramu[26], ramf[26], flash[26], app[26];

#if UIEXT_OTA_ENABLED
    snprintf(fw, sizeof(fw), "Firmware: %s", ota_fw_version_string());
#else
    snprintf(fw, sizeof(fw), "Firmware: v%d.%d.%d", UIEXT_FW_VERSION_MAJOR,
             UIEXT_FW_VERSION_MINOR, UIEXT_FW_VERSION_PATCH);
#endif

#if defined(PICO_RP2350) || defined(PICO_RP2350A) || defined(PICO_RP2350B)
    const char *chip = "RP2350";
#else
    const char *chip = "RP2040";
#endif
    snprintf(cpu, sizeof(cpu), "CPU: %s x2", chip);
    snprintf(clk, sizeof(clk), "Clock: %lu MHz",
             (unsigned long)(clock_get_hz(clk_sys) / 1000000u));

    float t = read_temp_c();
    int tenths = (int)(t * 10.0f + (t < 0 ? -0.5f : 0.5f));
    snprintf(temp, sizeof(temp), "Temp: %d.%d C", tenths / 10, tenths < 0 ? -(tenths % 10) : tenths % 10);

    // Free = the heap→stack gap (minus any heap actually malloc'd; this firmware
    // is mostly static, so heap use is ~0). Used = total SRAM minus that free gap
    // (i.e. static data + reserved stack), which is the meaningful figure.
    uint32_t sram_total = (uint32_t)(&__StackTop) - UIEXT_SRAM_BASE;
    uint32_t heap_total = (uint32_t)(&__StackLimit - &__bss_end__);
    uint32_t heap_used = (uint32_t)mallinfo().uordblks;
    uint32_t ram_free = heap_total > heap_used ? heap_total - heap_used : 0;
    uint32_t ram_used = sram_total > ram_free ? sram_total - ram_free : 0;
    snprintf(ramu, sizeof(ramu), "RAM used: %lu KB", (unsigned long)(ram_used / 1024u));
    snprintf(ramf, sizeof(ramf), "RAM free: %lu KB", (unsigned long)(ram_free / 1024u));

    uint32_t appsz = (uint32_t)(&__flash_binary_end - &__flash_binary_start);
    snprintf(flash, sizeof(flash), "Flash: %lu MB",
             (unsigned long)(PICO_FLASH_SIZE_BYTES / (1024u * 1024u)));
    snprintf(app, sizeof(app), "App: %lu KB", (unsigned long)(appsz / 1024u));

    printf("[sysinfo] %s | %s | %s | %s | %s | %s | %s | %s\n",
           fw, cpu, clk, temp, ramu, ramf, flash, app);

    const char *lines[] = { fw, cpu, clk, temp, ramu, ramf, flash, app };
    uiext_report_view("System Info", lines, (int)(sizeof(lines) / sizeof(lines[0])));
}
