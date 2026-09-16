# MicroPicoDrive-NG firmware

An internal Sinclair QL Microdrive replacement running on a Raspberry Pi Pico.
The board fits inside the QL and answers the microdrive bus in real time, so
the QL loads and saves as if a real cartridge were spinning. Cartridge images
live on an SD card and are chosen from a colour display on the front of the
unit.

See the [owner's manual](docs/USER_MANUAL.md) ([complete PDF](docs/USER_MANUAL.pdf),
[firmware-only PDF](docs/FIRMWARE_MANUAL.pdf),
[hardware-installation PDF](docs/HARDWARE_INSTALLATION_MANUAL.pdf)) for the installation outline,
SD card preparation, everyday use and the complete System Tools reference.
The physical fitting details still need confirmation for the NG board revision.

Core 0 emulates the microdrive protocol using the RP2040/RP2350 PIO state
machines, DMA and an event queue. Core 1 runs everything the user touches: the
ST7735 80x160 display, the four buttons, the SD card and the firmware updater.
The selected cartridge image is held entirely in RAM while it is mounted,
which is why the RAM figures below matter.

## Relationship to the original project

This firmware descends from [gusmanb/micropicodrive](https://github.com/gusmanb/micropicodrive),
which is the original internal QL microdrive replacement and the source of the
hardware concept, the microdrive protocol emulation and the cartridge formats.
That project is MIT licensed. Credit for the idea and the protocol work belongs
there.

This repository is a firmware fork focused on the user interface and on
updating the device in the field. What it adds:

- A menu driven colour UI on the ST7735 panel: a file browser with directory
  navigation, a cartridge screen, System Tools, and status and confirmation
  dialogs.
- An auto-load tag. Long press SELECT on an image to mark it, and that image
  mounts automatically at the next boot.
- Field firmware updates without a PC. Drop an update on the SD card, power on,
  and answer the prompt.
- A second hardware target. The Pico 2 W build adds Bluetooth Low Energy for
  wireless firmware updates and A/B firmware slots, so a failed update leaves
  the previous firmware bootable.
- Wireless cartridge management on the Pico 2 W: a paired phone or PC can
  browse the SD card, upload and download images, rename files and edit
  cartridge labels over BLE. The wire protocol is documented in
  [docs/BLE_PROTOCOL.md](docs/BLE_PROTOCOL.md), and `tools/mdvtool.py` is a
  working command line client.
- Haptic feedback from a vibration motor: distinct cues for cartridge
  insert and eject, finished transfers, errors (a double buzz) and the
  long-press threshold, and the motor runs while a cartridge image loads or
  saves. Every cue can be switched off individually in System Tools.
- Guards against data loss: eject is refused while the QL is using the drive, a
  cartridge with unsaved changes asks before ejecting, and saving is blocked if
  the SD card was swapped after the image was loaded.
- Persistent settings (theme, caption position, path bar, corner marker, and
  the Motor haptic options) stored in flash.

The upstream README does not document its firmware update mechanism or its UI
workflow, so this list describes what this firmware does rather than claiming
what the original does or does not do.

## The two builds

One source tree builds both images. The board is chosen at compile time and
decides which features are compiled in.

```
cmake -DPICO_BOARD=pico      ->  MicroPicoDrive-Lite   (Raspberry Pi Pico, RP2040)
cmake -DPICO_BOARD=pico2_w   ->  MicroPicoDrive        (Raspberry Pi Pico 2 W, RP2350)
```

| | Lite (RP2040) | Pico 2 W (RP2350) |
|---|---|---|
| Bluetooth | none | BLE: pairing, firmware upload, SD file management |
| Firmware slots | single image plus a staging area | A/B slots, bootrom picks the newest valid one |
| SD update file | one `.uf2` | `.bin` plus a `.json` manifest, verified by SHA-256 |
| USB update | merged image via BOOTSEL | sealed image via `picotool` |
| BOOTSEL from the menu | yes, in System Tools | no, the A/B slots cover recovery |
| Menu scroll animation | off | on |
| Settings storage | last flash sector | data partition |
| Version line | shared, v2.6.0+ | shared, v2.6.0+ |
| Static RAM used | 217,100 bytes of a 262,144 byte region | 291,428 bytes |

The differences come from two constraints rather than from taste.

The plain Pico has no radio, so everything BLE is absent from that image. It
also has far less RAM. Since a mounted cartridge image sits in RAM, the
remaining space is tight, and the vertical scroll animation needs a 32,000 byte
scratch buffer that does not fit. On the Lite build the spare RAM also serves
as the heap that FatFs allocates from while reading long filenames.

The RP2350 supports A/B partitions in its bootrom, which is what makes wireless
updating safe: the new image is written to the inactive slot and only becomes
bootable once it verifies. The RP2040 has no equivalent, so the Lite build
carries a small first stage loader in the first 16 KB of flash. An SD update is
staged into a separate flash region, and the loader applies it on the next
boot. If anything interrupts the process, the loader still has a complete
previous image to fall back on.

Which one to build depends on the board you fitted. The Lite build exists so
the project still works on a plain Pico, which is cheaper and easier to source
than a Pico 2 W.

## Controls

| Key | Browser | Cartridge screen | Elsewhere |
|---|---|---|---|
| K1 (UP) | move up | eject (confirm if unsaved changes) | dialog: toggle Yes/No |
| K2 (DOWN) | move down | eject (same confirm) | dialog: toggle Yes/No |
| K3 (SELECT) | open dir / mount file; long press = auto-load tag | save cartridge to SD | dialog: confirm; BLE modes: cancel |
| K4 (TOOLS) | System Tools | "Eject first" notice (tools need an ejected cartridge) | idle screens: System Tools; BLE modes: cancel; long press = BLE Connect (Pico 2 W) |

Notes:

- **Auto-load tag**: long press K3 on a file to mark it in `CONFIG.CFG`. The red
  bar in the left gutter shows the tagged file, which mounts automatically at
  boot. Long press again to untag. `CONFIG.CFG` is only created when you first
  tag something.
- **Eject while the QL uses the drive**: all keys are ignored during drive
  activity. Eject resumes once the QL deselects the drive.
- **Save onto a swapped card**: if the SD card was exchanged after the cartridge
  was loaded, SAVE refuses with "Different card / Save blocked". The image in
  RAM and its unsaved changes are kept. Reinsert the original card and save
  again.
- **Directory listing cap**: a folder shows at most 64 entries. If more exist,
  the list ends with a non-selectable `...` row. Which entries are dropped
  follows on-card order, not the alphabetical display order.
- **"Event lost" screen**: the emulated cartridge can no longer be trusted, so
  K1 (eject) is deliberately the only working key. Saving an untrusted image is
  blocked by design.

System Tools also holds an SD card check, a System Info page, and an
LED & Motor Test that blinks the activity LED and runs the vibration motor for
five seconds. Its Motor section controls the haptics: a global motor switch,
one toggle per cue (cartridge, transfer, alerts, long press), and the
load/save motor run with a selectable spin-down tail (off, 250 ms, 500 ms
or 1 s).

## Hot-swap handling

The cartridge can be inserted and removed with the QL powered. The board edge
has staggered fingers (see the
[hardware repository](https://github.com/arleybls/micropicodrive-ng-hardware)):
GND and the detect line mate first, +3V3 second, and the signal fingers last.
The firmware supervises the detect line (GPIO 15, low = cartridge present)
around that mating order:

- **Insertion** is debounced: the detect line must stay low for 250 ms
  continuously before any cartridge-facing pin is driven and the display and
  SD card are initialised. The first low edge only proves the longest finger
  touched — not that power and signals are seated.
- **Removal** is acted on immediately: a rising edge on detect (confirmed
  against glitches over a short window) tri-states every cartridge-facing pin
  within a few milliseconds, marks the SD volume unmounted, and re-arms the
  insertion debounce.
- **Rip-out backup**: because detect breaks last, a fast pull can cut power and
  signals while detect still reads present. A streak of consecutive SD errors
  is therefore treated as a probable removal — the lines are tri-stated and
  presence is re-evaluated, which also recovers a badly seated cartridge.

## Firmware update

The device updates itself. You do not need a programmer, and on the normal
routes you do not need to open the QL. Two methods work without a PC connected
to the board: dropping a file on the SD card, and, on the Pico 2 W, sending the
firmware over Bluetooth.

### Dropping a file on the SD card

Works on both builds and needs nothing but the card.

Put the firmware in a folder named `.update` at the root of the card. The
folder is hidden from the file browser, so it will not clutter your cartridge
list. Which file depends on the board:

| Build | What to drop in `\.update\` |
|---|---|
| Lite | the `.uf2` |
| Pico 2 W | the `.bin` and its matching `.json` manifest, both |

A card prepared for one build does nothing on the other. The two updaters read
different file types and each ignores what it does not recognise.

Put the card back in the drive and power on. The device checks the folder
before it loads any cartridge, so the flash write can never collide with the QL
using the drive. If the version on the card differs from the version running,
you get a prompt showing both, with "(older)" marking a downgrade.

Answer with the buttons:

- **Yes** installs it.
- **No** declines and deletes the update files, so you are not asked again.
- **Waiting** leaves the files alone. The prompt clears itself after 30 seconds
  and you are asked again at the next boot. Every confirmation dialog in the
  firmware behaves this way, timing out to the answer that changes nothing.

Downgrades are allowed deliberately. Physical access to the card counts as
permission, so you can always put an older version back.

What happens next differs by board, though both show progress on screen and
reboot when finished. The Lite build reads the file into a staging area and
writes the marker that arms it only once the whole image is present, then
restarts so the first stage loader can apply it. The Pico 2 W writes into
whichever firmware slot is not currently running and holds back the first
sector until the image passes a SHA-256 check against the manifest, then
reboots into it. In both cases an interruption, a power cut or a corrupt file
leaves the previous firmware intact and bootable.

### Over Bluetooth (Pico 2 W only)

Eject the cartridge, then open System Tools and choose Update Firmware. The
radio only ever runs in these explicit menu modes with no cartridge mounted,
which is what keeps it away from the timing-critical protocol emulation.

The device advertises and waits. Send the `.bin` and its `.json` manifest from
a paired host. The transfer shows a progress counter, and the image is verified
before anything becomes bootable, exactly as on the SD route.

Two behaviours worth knowing:

- **The version must be strictly newer than what is running.** Reusing a
  version number is rejected, because the comparison is on the number alone and
  cannot see that the binary changed.
- **If you walk away from the install prompt**, the verified image is not
  discarded. It is written to `\.update\` on the SD card and offered again at
  the next boot. Answering No discards it.

If an update leaves the device misbehaving, System Tools offers Revert Firmware
while the other slot still holds a working image.

The firmware-upload client lives in the companion app; this repository does
not ship one. It does ship `tools/mdvtool.py`, a command line BLE client for
the file side — browse, upload, download, rename and cartridge labels, from
the Connect mode in System Tools — and
[docs/BLE_PROTOCOL.md](docs/BLE_PROTOCOL.md), which documents the wire
protocol for anyone writing their own client.

### USB BOOTSEL

The fallback route, and the one to use on a blank board. Hold BOOTSEL while
plugging in the USB cable and the board appears as a drive called `RPI-RP2`.

For the Lite build, copy the merged image, which contains the first stage
loader, the application and a segment that clears any pending staged update:

```
MicroPicoDrive-full.uf2  ->  RPI-RP2
```

Copy that file rather than the application UF2 on its own. The application is
linked above the loader and will not boot by itself on a blank board.

For the Pico 2 W build, use `picotool` from the Pico SDK:

```
picotool load -v -x MicroPicoDrive_v<M.m.p>.uf2
```

A first install on the Pico 2 W also needs the partition table and the radio
firmware written once, before the application image;
`tools\install_mainline.ps1` runs that whole sequence against a board waiting
in BOOTSEL mode.

## Building

Needs the Raspberry Pi Pico SDK 2.2.0 with CMake and Ninja. Both scripts pass
the board explicitly, which matters because `PICO_BOARD` is a CMake cache
variable and the build directories persist between runs.

```powershell
powershell -File tools\build_lite.ps1 -Major 2 -Minor 10 -Patch 1
powershell -File tools\build_ota.ps1  -Major 2 -Minor 10 -Patch 1
```

Both builds share one version line since v2.6.0: a release always covers both
boards, because a change to the shared tree rebuilds both images.

Any board other than `pico` or `pico2_w` stops the configure step with an
error rather than producing an image that resembles one of the two.

Patch numbers are capped at 0 to 98 — 99 is reserved for the OTA-test image
that `build_ota.ps1` emits alongside each release. The Pico 2 W firmware seal
and the BLE protocol carry only a major and a minor field, so ordering there
uses `minor * 100 + patch`.

### Editor setup

The build does not depend on an editor, and no editor configuration is kept in
this repository. The Raspberry Pi Pico extension writes its own `.vscode`
files, and those pin the exact SDK, toolchain, CMake, Ninja, picotool and
OpenOCD versions installed on the machine that generated them, so a committed
copy would point at directories another contributor does not have.

If you use VS Code, these are the extensions this project was developed with:

| Extension ID | Purpose |
|---|---|
| `raspberry-pi.raspberry-pi-pico` | Installs and manages the Pico SDK and toolchain, and generates the editor configuration |
| `ms-vscode.cpptools` | C and C++ language support and IntelliSense |
| `ms-vscode.cpptools-extension-pack` | Bundle that pulls in the C and C++ tooling |
| `ms-vscode.cmake-tools` | CMake integration for configuring and building |
| `marus25.cortex-debug` | On-chip debugging over SWD |
| `ms-vscode.vscode-serial-monitor` | Serial output from the board |

Install the Pico extension first and let it set up the SDK. The rest of the
configuration follows from that.

## Licence and credits

Copyright (C) 2026 Arley Silveira.

This program is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License, version 3**, as published by the
Free Software Foundation. It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the full text.

GPL-3.0 is not an arbitrary choice. The vendored ST7735 display driver traces
back through [bablokb/pico-st7735](https://github.com/bablokb/pico-st7735) to
[gavinlyonsrepo/pic_16F18346_projects](https://github.com/gavinlyonsrepo/pic_16F18346_projects),
which is GPL-3.0. Because that code is copyleft, the combined work has to be
distributed under the same terms. The details are recorded in
[src/lib-st7735/LICENSE](src/lib-st7735/LICENSE).

### Credits

This firmware is a derivative of
[gusmanb/micropicodrive](https://github.com/gusmanb/micropicodrive) by
Agustín Gimenez Bernad, which is MIT licensed. MIT code may be incorporated
into a GPL-3.0 work, and the original copyright notice is retained.

Vendored third party components and their licences:

| Path | Component | Licence |
|---|---|---|
| `src/lib-sdcard/` | carlk3 no-OS-FatFS-SD | Apache 2.0 |
| `src/lib-sdcard/src/ff15/` | FatFs by ChaN | own permissive licence, see its `LICENSE.txt` |
| `src/lib-st7735/` (driver) | Bernhard Bablok's pico-st7735, after Gavin Lyons | GPL-3.0 |
| `src/lib-st7735/` (fonts) | Adafruit GFX fonts | BSD, Copyright (c) 2012 Adafruit Industries |
| `src/ble/jsmn.h` | jsmn JSON tokenizer, zserge v1.1.0 | MIT |
| `src/ui/Roboto_Thin_8.c` | font data rasterized from Roboto Thin | Roboto is Apache 2.0 |

If you are the author of any vendored component and the attribution here is
wrong or incomplete, please open an issue and it will be corrected.
