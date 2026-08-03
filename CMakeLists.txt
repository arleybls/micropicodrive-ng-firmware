# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(src/pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(MicroPicoDrive C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# SD card + full FatFs (vendored carlk3 no-OS-FatFS from the UIExt sandbox,
# commit 057b1ae; INTERFACE target)
add_subdirectory(src/lib-sdcard/src)

# ST7735 display library (vendored from the UIExt sandbox, commit 057b1ae).
# No TFT_ENABLE_RESET: this board has no RST net (RC auto-reset + SWRESET).
add_subdirectory(src/lib-st7735)
set(MPD_TFT_DEFINES
        TFT_ENABLE_BLACK
        TFT_ENABLE_FONTS
        TFT_ENABLE_SHAPES
        TFT_ENABLE_ROTATE
        SPI_TFT_PORT=spi1
        SPI_TFT_CS=20
        SPI_TFT_DC=21)
target_compile_definitions(st7735 PRIVATE ${MPD_TFT_DEFINES})

# One tree, two products (docs/SINGLE_TREE_STUDY.md):
#   pico2_w -> "MicroPicoDrive"      RP2350, BLE/OTA, A/B partitions
#   pico    -> "MicroPicoDrive-Lite" RP2040, radio-less, flashloader + flat UF2
# Anything else is a mistake, not a variant — fail loudly rather than emit an
# image that looks like one of the two.
if(PICO_BOARD STREQUAL "pico2_w")
    set(UIEXT_OTA_ENABLED 1)
elseif(PICO_BOARD STREQUAL "pico")
    set(UIEXT_OTA_ENABLED 0)
else()
    message(FATAL_ERROR
        "PICO_BOARD='${PICO_BOARD}' is not supported. Use -DPICO_BOARD=pico2_w "
        "(mainline) or -DPICO_BOARD=pico (Lite). NOTE: PICO_BOARD is a CACHE "
        "variable — a build dir last configured for the other board keeps it.")
endif()

# Vertical scroll animation: a RAM question, NOT a radio question. Its
# scratch buffer is 32,000 bytes of bss and the RP2040's spare RAM doubles as
# the FatFs heap. Kept separate from UIEXT_OTA_ENABLED on purpose — see
# study trap T8.
if(PICO_BOARD STREQUAL "pico2_w")
    set(UIEXT_VSCROLL_ANIM_ENABLED 1)
else()
    set(UIEXT_VSCROLL_ANIM_ENABLED 0)
endif()

# Display settings persist on BOTH boards, in different places (study trap T1):
#   OTA build  -> data partition at 0x344000
#   Lite build -> last flash sector (flash_layout.h, FL_SETTINGS_OFFSET)
# Always on: gating this on the radio is what silently removed the feature.
set(UIEXT_SETTINGS_PERSIST 1)

message(STATUS "MicroPicoDrive: board=${PICO_BOARD} OTA/BLE=${UIEXT_OTA_ENABLED} "
               "vscroll=${UIEXT_VSCROLL_ANIM_ENABLED}")

# Add executable. Default name is the project name, version 0.1

set(MPD_SOURCES
        src/MicroPicoDrive.c
        src/drive/MicroDriveControl.c
        src/drive/EventMachine.c
        src/drive/SharedBuffers.c
        src/ui/UserInterface.c
        src/ui/UserInterfaceExtension.c
        src/ui/Roboto_Thin_8.c
        src/ui/sd_thumb.c
        src/ui/sys_info.c
        src/storage/sd_check.c
        src/storage/sd_hw_config.c)
if(UIEXT_OTA_ENABLED)
    # BLE/OTA stack vendored from the UIExt sandbox, commit 057b1ae (plus
    # btstack_config.h, ota.gatt, jsmn.h, boards/partition_table.json).
    list(APPEND MPD_SOURCES
            src/ble/ota_ble.c
            src/ble/ota_btstack_glue.c
            src/ble/ota_flash_bank.c)
    # A/B-slot updater (bin+json, SHA-256).
    list(APPEND MPD_SOURCES src/update/sd_update_ota.c)
else()
    # Flashloader-staged updater (UF2, CRC-32). Exactly one of the two is
    # compiled — neither self-gates, so a wrong selection is a link error
    # rather than a silently missing feature. See sd_update.h.
    list(APPEND MPD_SOURCES src/update/sd_update_lite.c)
endif()
add_executable(MicroPicoDrive ${MPD_SOURCES})

# Firmware identity: MicroPicoDrive starts at v2.0 with the new UI stack.
# Three-part versions since v2.5.1; the A/B seal and the BLE wire carry only
# major/minor u16, so ordering uses minor*100+patch there (patch 0-99).
set(MPD_FW_VERSION_MAJOR 2 CACHE STRING "Firmware major version")
set(MPD_FW_VERSION_MINOR 0 CACHE STRING "Firmware minor version")
set(MPD_FW_VERSION_PATCH 0 CACHE STRING "Firmware patch version")
target_compile_definitions(MicroPicoDrive PRIVATE
        ${MPD_TFT_DEFINES}
        UIEXT_FW_VERSION_MAJOR=${MPD_FW_VERSION_MAJOR}
        UIEXT_FW_VERSION_MINOR=${MPD_FW_VERSION_MINOR}
        UIEXT_FW_VERSION_PATCH=${MPD_FW_VERSION_PATCH}
        UIEXT_OTA_ENABLED=${UIEXT_OTA_ENABLED}
        UIEXT_VSCROLL_ANIM_ENABLED=${UIEXT_VSCROLL_ANIM_ENABLED}
        UIEXT_SETTINGS_PERSIST=${UIEXT_SETTINGS_PERSIST})

# Create C header file with the name <pio program>.pio.h
pico_generate_pio_header(${PROJECT_NAME}  
        ${CMAKE_CURRENT_LIST_DIR}/src/drive/PIO_machines.pio
)

# Per board: this is the only way to tell two UF2s apart in picotool, and
# dist/ holds both. Never appears on the display — the boot title is
# "MicroPicoDrive" on both.
if(UIEXT_OTA_ENABLED)
    pico_set_program_name(MicroPicoDrive "MicroPicoDrive")
else()
    pico_set_program_name(MicroPicoDrive "MicroPicoDrive-Lite")
endif()
pico_set_program_version(MicroPicoDrive "${MPD_FW_VERSION_MAJOR}.${MPD_FW_VERSION_MINOR}.${MPD_FW_VERSION_PATCH}")

pico_enable_stdio_uart(MicroPicoDrive 0)
pico_enable_stdio_usb(MicroPicoDrive 0)

# Add the standard library to the build
target_link_libraries(MicroPicoDrive
        pico_stdlib
        pico_multicore)

# Add the standard include files to the build
# Every src/ group is on the include path, so sources keep including their
# siblings by plain name (#include "UserInterface.h") regardless of which
# group a header lives in.
target_include_directories(MicroPicoDrive PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/src/drive
  ${CMAKE_CURRENT_LIST_DIR}/src/ui
  ${CMAKE_CURRENT_LIST_DIR}/src/storage
  ${CMAKE_CURRENT_LIST_DIR}/src/update
  ${CMAKE_CURRENT_LIST_DIR}/src/ble
  ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
)

# Add any user requested libraries
target_link_libraries(MicroPicoDrive
        hardware_spi
        hardware_pio
        hardware_dma
        hardware_timer
        hardware_pwm
        hardware_adc
        hardware_clocks
        st7735
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

# Flash access is needed on BOTH boards: settings persistence (both), the SD
# updater (both), BOOTSEL (Lite). These used to sit inside the OTA block,
# which is why re-enabling any of those features failed to link.
target_link_libraries(MicroPicoDrive
        hardware_flash
        hardware_watchdog
        pico_flash
        pico_bootrom)

if(UIEXT_OTA_ENABLED)
    # BLE OTA — NOTE: pico_btstack_cyw43 deliberately NOT linked; its
    # btstack_cyw43.c + flash bank hang under partition boot (QMI address
    # translation). Replaced by ota_btstack_glue.c + ota_flash_bank.c.
    target_link_libraries(MicroPicoDrive
            pico_cyw43_arch_none
            pico_btstack_ble
            pico_btstack_hci_transport_cyw43
            pico_btstack_run_loop_async_context
            pico_sha256
            pico_unique_id
            )

    # GATT database for the OTA service (generates the ota.h the code includes)
    pico_btstack_make_gatt_header(MicroPicoDrive PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/ble/ota.gatt")

    # CYW43 Wi-Fi/BT firmware lives in its own flash partition (see
    # partition_table.json, family "cyw43-firmware"). Embedding it in the app
    # binary hangs cyw43_arch_init when the app boots from an A/B partition
    # (QMI address translation) — sandbox hardware finding 2026-07-18.
    # NO_EMBEDDED_PT: we manage the flash partition table ourselves.
    pico_use_wifi_firmware_partition(MicroPicoDrive NO_EMBEDDED_PT)
else()
    # Flashloader layout (see flash_layout.h): the app is linked at
    # XIP_BASE+0x4000; the flashloader below owns the first 16 KB and applies
    # staged SD updates. Study trap T9: mainline's CMakeLists had deleted this
    # whole pipeline, so a -DPICO_BOARD=pico build SUCCEEDED but produced an
    # app linked at flash origin 0 with no flashloader — a silent mismatch
    # against any field unit running the Lite layout.
    pico_set_linker_script(MicroPicoDrive ${CMAKE_CURRENT_LIST_DIR}/boards/memmap_app.ld)
endif()

pico_add_extra_outputs(MicroPicoDrive)

if(NOT UIEXT_OTA_ENABLED)
    # Flashloader: tiny first-stage image at flash start (default linker
    # script), applies staged SD updates before jumping to the app.
    add_executable(flashloader src/flashloader/flashloader.c)
    # src/update carries flash_layout.h, which the loader and the app must
    # agree on: it defines where the staged image lives.
    target_include_directories(flashloader PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_LIST_DIR}/src/update)
    # No alarm pool: nothing in the loader sleeps, and it keeps the timer IRQ
    # out of the window between our NVIC quiesce and the app's crt0.
    target_compile_definitions(flashloader PRIVATE PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1)
    target_link_libraries(flashloader pico_stdlib pico_bootrom hardware_flash)
    # FLASH capped at 16 KB: outgrowing the region is a link error, not a
    # silent overlap with the app at +0x4000.
    pico_set_linker_script(flashloader ${CMAKE_CURRENT_LIST_DIR}/src/flashloader/memmap_flashloader.ld)
    pico_set_program_name(flashloader "MicroPicoDrive-Lite-flashloader")
    pico_enable_stdio_uart(flashloader 0)
    pico_enable_stdio_usb(flashloader 0)
    pico_add_extra_outputs(flashloader)

    # Merged UF2 (flashloader + app + staging-header clear) — the ONLY correct
    # BOOTSEL/first-install image with this layout. UF2 blocks are
    # self-addressed, so byte concatenation is a valid merge. The stage_clear
    # segment zeroes the staging header, so a full reflash (rollback) disarms
    # any pending staged update.
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/stage_clear.uf2
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_LIST_DIR}/tools/make_stage_clear.ps1"
                    -Out "${CMAKE_BINARY_DIR}/stage_clear.uf2"
            DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/make_stage_clear.ps1
            COMMENT "Generating stage_clear.uf2")
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/MicroPicoDrive-full.uf2
            COMMAND cmd /c copy /b flashloader.uf2+MicroPicoDrive.uf2+stage_clear.uf2 MicroPicoDrive-full.uf2 > nul
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS MicroPicoDrive flashloader ${CMAKE_BINARY_DIR}/stage_clear.uf2
            COMMENT "Merging flashloader + app + stage_clear -> MicroPicoDrive-full.uf2 (BOOTSEL image)")
    add_custom_target(full_uf2 ALL DEPENDS ${CMAKE_BINARY_DIR}/MicroPicoDrive-full.uf2)
endif()

