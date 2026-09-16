# MicroPicoDrive NG firmware manual

![](images/rainbow.png)

This manual shows you how to prepare a microSD card, work with cartridge
images on the Sinclair QL, and set everything up in System Tools. It matches
firmware version 2.10.1.

Fitting the board inside the QL — including the Pico and the optional
vibration motor — is covered by the hardware installation manual in the
[hardware repository](https://github.com/arleybls/micropicodrive-ng-hardware).

## Contents

- [The two parts and firmware versions](#the-two-parts-and-firmware-versions)
- [Preparing the microSD card](#preparing-the-microsd-card)
- [First start and everyday use](#first-start-and-everyday-use)
- [System Tools reference](#system-tools-reference)
- [Firmware updates](#firmware-updates)
- [Troubleshooting](#troubleshooting)
- [Addendum: creating images with Sinclair MDV Builder](#addendum-creating-images-with-sinclair-mdv-builder)
- [Addendum: the Bluetooth protocol for developers](#addendum-the-bluetooth-protocol-for-developers)

## The two parts and firmware versions

The **mainboard** stays inside the QL and connects to its Microdrive bus. It
carries the Raspberry Pi Pico that runs the firmware. The removable
**cartridge board** carries the display, four buttons and microSD socket.
It plugs into the mainboard's edge connector.

A **cartridge image** is a file on the microSD card that behaves like one QL
Microdrive cartridge. When you *mount* an image, the QL sees it as a cartridge
sitting in the drive; when you *eject* it, you are back in the file browser.
Mounting and ejecting happen on the screen — they are not the same thing as
physically plugging in or pulling out the cartridge board.

| Feature | Lite: Raspberry Pi Pico / RP2040 | Full: Raspberry Pi Pico 2 W / RP2350 |
|---|---|---|
| Image browsing, QL loading and saving | Yes | Yes |
| Display and motor settings | Yes | Yes |
| SD card firmware updates | UF2 file | BIN file and matching JSON manifest |
| Bluetooth file management and updates | No | Yes |
| Revert Firmware | No | When another firmware slot is available |
| BOOTSEL Mode menu entry | Yes | No |

Install the firmware that matches the Pico you actually fitted. The Pico 2 W's
wireless features use Bluetooth Low Energy (BLE), not Wi-Fi.

## Preparing the microSD card

### Format and copy files

1. Back up any existing files before formatting: formatting erases them.
2. Use a card reader on your computer. Prepare a single normal data volume in
   **FAT32 or exFAT**. Both are enabled in this firmware; FAT12 and FAT16 are
   also supported by its filesystem library. NTFS and APFS are not supported.
   Check the selected device carefully before applying the format.
3. Copy valid `.mdv` cartridge images onto the card. You can put them
   in the root or organise them into folders. Extract downloaded ZIP archives
   on the computer first.
4. Safely eject the card from the computer and insert it into the cartridge
   board's microSD socket in the socket's indicated orientation. Do not force it.

You don't need any firmware files or a `CONFIG.CFG` on the card just to browse
images. We haven't tested every card size, and a supported filesystem alone
doesn't guarantee a particular card will behave — so run
**System Tools > SD Check** once on a freshly prepared card.

Example card layout:

```text
/
  Games/
    CHESS.mdv
    CHESS.mdv.thumb       optional matching artwork
  Work/
    NOTES.mdv
  CONFIG.CFG             created when you tag an auto-load image
  .update/               needed only for firmware updates
```

The QL never sees this folder tree — it only sees the inside of the mounted
cartridge image. Copying a loose QL program or a ZIP file onto the card does
not put it inside a cartridge; use an image authoring tool on your computer
for that. The device itself cannot create new blank images. This project's
companion app for authoring `.mdv` images is
[Sinclair MDV Builder](https://github.com/arleybls/sinclair-mdv-builder) —
see the [addendum](#addendum-creating-images-with-sinclair-mdv-builder) at
the end of this manual for a quick reference.

### Image and filename limits

| Item | Requirement or behaviour |
|---|---|
| MDV image | 174,930 bytes |
| Browser file filter | `.mdv`, case-insensitive |
| Auto-load extension | Use all-lowercase or all-uppercase: `.mdv` or `.MDV`; mixed-case extensions do not auto-load in this version |
| File and folder names | Keep each to 63 bytes or fewer, including extension; longer names are truncated by the browser and may fail to open |
| Full path | Keep below 300 bytes, including separators; shorter paths also avoid thumbnail lookup limits |
| Entries per folder | At most 64 eligible files and folders; use subfolders for larger collections |
| Hidden entries | Hidden/system files and names starting with a dot are omitted |

Use short, simple filenames when preparing your first card. An image with the
wrong size reports **Bad format**. Renaming an unrelated file to `.mdv` does
not convert it into a cartridge image.

When a folder exceeds the entry limit, a non-selectable `...` row appears.
The firmware collects entries in on-card order before sorting them for display,
so the omitted files are not necessarily the last alphabetically.

Optional artwork lives beside the image. The preferred `CHESS.mdv.thumb`
format is a raw 160 by 80 pixel RGB565 big-endian image, exactly 25,600 bytes.
The firmware also accepts legacy `CHESS.thumb` artwork as uncompressed 24-bit
BMP data. A renamed JPEG or PNG will not work. Missing artwork does not prevent
the cartridge from loading.

## First start and everyday use

### First-start check

1. Start with a card containing at least one known-good image and no firmware
   update files. Insert the cartridge board and power on the QL.
2. Wait for the display to initialise and the file browser to appear. Without
   a readable card, the firmware shows a waiting screen instead.
3. Use K1/K2 to highlight an image and press K3 to mount it. Wait for loading
   to finish and the cartridge screen to appear. (See Buttons below for the
   full button map.)
4. At the QL's SuperBASIC prompt, list the corresponding drive. For a unit
   installed as drive 1:

   ```basic
   DIR mdv1_
   ```

   Use `DIR mdv2_` if it replaces drive 2. A directory listing confirms the QL
   can read the mounted image. If it fails, check the image and drive position
   before investigating the physical installation with power disconnected.

Before you save any real work, read **Loading programs and saving your work**
below — saving on the QL and saving to the SD card are two separate steps.

### Buttons

The button labels are K1 through K4; a long press is approximately one second.

| Button | File browser | Mounted cartridge screen | System Tools / reports |
|---|---|---|---|
| K1 / UP | Previous entry | Eject image | Previous item / scroll up |
| K2 / DOWN | Next entry | Eject image | Next item / scroll down |
| K3 / SELECT | Open folder or mount image | Save image to SD | Activate item or cycle its value; close a report |
| K4 / TOOLS | Open System Tools | Shows `Eject first` | Leave menu or close report |
| Hold K3 | Tag or untag image for auto-load | No special save action | On `Pair Device`, request forgetting all pairings |
| Hold K4 | Pico 2 W: shortcut to Connect | Eject first | Use the displayed mode's controls |

Folders appear in brackets. Select `[..]` to go up one folder.
During QL drive activity the normal cartridge buttons are ignored; wait for
the QL operation to finish. The activity LED indicates drive selection/activity
and also blinks during SD operations. It does not distinguish QL reads from writes.

In Yes/No dialogs, K1 or K2 changes the answer and K3 confirms it. Confirmation
dialogs time out after about 30 seconds without carrying out the requested
action. Firmware update timeout handling is explained below.

### Loading programs and saving your work

Once mounted, use the image like a Microdrive cartridge. For example,
`LOAD mdv1_program` loads a BASIC program named `program`; some application
cartridges use `LRUN mdv1_boot`. Follow the program's own loading instructions
and substitute the correct drive number.

**Saving on the QL and saving to the SD card are two separate steps.** QL
writes change the mounted image in RAM. They are not automatically written
back to the card.

1. Save your work from the QL application, or use a command such as
   `SAVE mdv1_test` for a BASIC program.
2. Wait until the QL has finished using the drive.
3. Press **K3 on the cartridge screen**. Wait for **Saved** before removing
   anything or turning off power. This overwrites the original image file.
4. Press K1 or K2 to eject the image when you want to choose another one.

If you eject after QL writes without saving to SD, the device asks
**Eject? / Unsaved changes**. **Yes discards those changes**; it does not save
them. Choose No, press K3 to save, then eject again to keep your work.

Keep backups on your computer. Saving rewrites the image in place, so a failed
or interrupted SD save can leave the on-card file incomplete. If you see
**Save failed / Retry save!**, keep the unit powered and the image mounted,
resolve the card problem and retry K3. The firmware retains the RAM image after
a failed save, but ejecting or losing power loses that recovery opportunity.

### Auto-load a favourite image

In the browser, highlight an image and hold K3 for about one second. A red
marker in the left gutter identifies the tagged image. Only one image per
card can be tagged; tagging another replaces the previous choice. Hold K3 on
the tagged image again to clear the tag.

The firmware creates or updates `CONFIG.CFG` in the card root, recording the
image's path in a `FILE=` entry. It tries that image when the card is mounted
during startup. Tagging does not run a QL program; it only mounts the cartridge.
If you rename or move the image, tag it again at its new location. Use the
buttons to manage this file rather than editing it by hand.

### Removing a card or cartridge board

Save to SD, eject the image, and wait for any file operation to finish before
removing the SD card or cartridge board. The cartridge board supports powered
insertion/removal, but this does not preserve unsaved work or make interruption
of an SD write safe. Never remove it during a firmware update.

If the SD card was swapped while an image remained mounted, saving may report
**Different card / Save blocked**. Reinsert the original card and retry K3;
the RAM image is retained. This protection compares volume serial numbers,
so it is not a substitute for the save-and-eject procedure.

## System Tools reference

Eject the cartridge image, then press K4 in the browser or an available waiting
screen. K1/K2 moves through the list, K3 activates an item or cycles its value,
and K4 or **Exit** leaves the menu. **UI Options** and **Motor** are section
labels, not actions.

Display and motor changes take effect in the session and are saved to onboard
flash when you leave System Tools. Exit the menu before powering off. These
settings stay with the mainboard when you change SD cards; the auto-load tag
stays on each card in `CONFIG.CFG`.

### Diagnostics and maintenance

| Menu item | Availability | What it does |
|---|---|---|
| SD Check | Both builds; only if a card mounted when Tools opened | Reports filesystem, total/free space, and write/read speeds using a 32 KB test file with sampled data verification |
| System Info | Both | Shows firmware version, CPU type, clock, chip temperature, RAM used/free, flash capacity and application size |
| LED & Motor Test | Both | Blinks the activity LED and runs the fitted motor for five seconds; intentionally bypasses the Motor on/off setting |
| BOOTSEL Mode | Lite only | After confirmation, reboots into USB programming mode; normal drive operation stops |
| Exit | Both | Leaves Tools and saves changed display/motor settings |

**SD Check writes to the card.** It creates or overwrites `UIEXTTST.BIN` in the
root and deletes it when the read/check sequence completes. Do not keep your
own file under that name. A failed test can leave it behind. The test is a
quick access check, not a full-card surface test or filesystem repair.

Use K1/K2 to scroll diagnostic reports and K3/K4 to return. If you inserted a
card while Tools was already open, exit and reopen Tools to make SD Check
available. System Info temperature is the Pico's sensor reading, not a
measurement of the whole QL.

### UI Options

Press K3 repeatedly to cycle a setting. The label shows the current value.

| Setting | Values | Default without saved settings | Effect |
|---|---|---|---|
| Caption | Top, Mid, Bottom | Bottom | Positions the caption on the cartridge artwork screen |
| Dark Mode / Light Mode | Dark or Light | Dark | Switches display background/text and selection colours |
| Path Bar | On, Off | Off | Shows or hides the red path title bar in the file browser |
| Rainbow | Top Left, Top Right, Btm Left, Btm Right, Off | Top Right | Positions or hides the Sinclair rainbow corner decoration |

### Motor

These options only do something when the optional vibration motor is fitted.
They never affect the emulation or your saved cartridge data.

| Setting | Values / default | Effect |
|---|---|---|
| Motor | On / Off; default On | Master switch for normal vibration feedback |
| Cart | On / Off; default On | Feedback for physical cartridge-board insertion/removal detection |
| Transfer | On / Off; default On | Completion feedback for supported Bluetooth transfer/update operations |
| Alerts | On / Off; default On | Double-buzz feedback for error/alert events |
| Long Press | On / Off; default On | Feedback when a button reaches its long-press threshold |
| Load/Save | Off, 250ms, 500ms, 1s; default 500ms | Runs the motor during SD image load/save and for the selected extra time afterwards |

The `Load/Save` time is how long the motor keeps running *after* the operation
finishes — a little spin-down, like a real drive. The motor always runs for
the whole load or save itself. Turning **Motor** off silences all feedback at
once while remembering your individual settings, and **LED & Motor Test**
still runs the motor regardless, so you can always check the hardware.

### Bluetooth and firmware slots: Pico 2 W only

| Menu item | What it does |
|---|---|
| Connect | Opens a BLE session for a previously paired client to manage SD files and query the device |
| Pair Device | Allows a new client to pair; use a short K3 press |
| Paired Devices | Lists saved pairings; select one with K3 to open `Forget Device?`, or K4 to return |
| Update Firmware | Opens the wireless firmware update mode |
| Revert Firmware | Appears when the other slot contains firmware; asks before switching back and rebooting |

The radio only runs while you are in one of these menu modes, and only with no
image mounted and the QL drive idle — during normal cartridge use it is
completely off. K3 or K4 cancels the connect, pairing and update waiting
screens; if a firmware installation is already running, let it finish.

Holding K3 on **Pair Device** requests **Forget all paired devices?**. Confirm
only if you want to remove every stored pairing. After forgetting a device,
remove its old pairing at the client too before pairing again.

Revert Firmware invalidates the current slot and boots the other one. It is
not a permanent toggle between two retained versions; reinstall the newer
firmware if you want to return to it later.

## Firmware updates

Use a release intended for your board. Save and eject your mounted image first,
and keep power and the cartridge board connected throughout installation.

### From the microSD card

1. On your computer, create a folder called `.update` at the card root.
2. Copy the release's SD-update files into it:

   | Build | Files |
   |---|---|
   | Lite / RP2040 | The release's update `.uf2` |
   | Pico 2 W / RP2350 | The `.bin` and its matching `.json` manifest |

3. Safely eject the card, fit it to the device and power on. The update check
   happens before image auto-load. `.update` is hidden from normal browsing.
4. Review the installed and offered versions, choose Yes or No with K1/K2,
   and confirm with K3.

**Yes** installs and reboots. **No deletes the offered update files**, preventing
another prompt. Waiting for the 30-second timeout leaves the files for another
offer at the next startup. A differing older version can be installed from SD;
the prompt marks it as older. Pico 2 W ignores a same-version SD update. Lite
skips an image whose contents already match the running application; a changed
image with the same version number can still be offered.

### Over Bluetooth

The usual way to update a Pico 2 W wirelessly is the **MicroPicoDrive
Companion** phone app: it finds the latest release, downloads and checks it,
and sends it to the unit. Pair the phone once, eject the mounted image, and
start the update from the app — on firmware 2.7.0 or later the app switches
the unit into Update Firmware mode by itself; on older firmware, choose
**System Tools > Update Firmware** on the unit first. Any client implementing
this project's BLE update protocol can do the same; this repository does not
include a dedicated firmware upload client.

**Quick reference: the MicroPicoDrive Companion app**

| When you want to... | Start in the app at |
|---|---|
| Connect your phone to the unit | Home > Connect to a unit |
| Update the unit's firmware | Home > Firmware |
| Send a cartridge to the unit | Local MDV Catalog > long-press a cartridge |
| Retrieve a cartridge from the unit | Remote MDVs > the retrieve button |
| Back up the whole unit into a folder | Remote MDVs > Back up |
| Create a blank cartridge on the SD card | Remote MDVs > Create empty |
| Reboot, identify or rename the unit | Home > Pico Tools |

Two things the app deliberately does not do: copying is not synchronizing —
changes made on the QL do not update the phone's copy, so retrieve the
cartridge again when you want the newer contents — and sending a cartridge
does not mount it; load it with the unit's own buttons afterwards.

The wireless route requires a strictly newer version. Confirm installation
when prompted. No discards the upload; if the prompt times out, the firmware
attempts to store the verified update in `.update` on the SD card for the next
boot. That deferred route requires a working writable card. Watch for any
reported storage error rather than assuming the update was saved.

### Initial programming and USB recovery

Initial programming differs from an ordinary SD update. For Lite, the merged
`MicroPicoDrive-full.uf2` includes the bootloader needed on a blank Pico. The
application UF2 alone is not a complete first installation. Hold the Pico's
BOOTSEL button while connecting USB to enter its `RPI-RP2` programming drive,
then copy the merged image. The Lite menu's **BOOTSEL Mode** also enters this
mode on an already running unit.

Pico 2 W first installation additionally needs its partition table and radio
firmware, followed by the sealed application image. Follow the release's
board-specific instructions and the repository's
[firmware and build documentation](../README.md#firmware-update). Confirm the
hardware's USB power arrangement before servicing an installed mainboard.

## Troubleshooting

| Symptom or message | What to check or do |
|---|---|
| No display | Check cartridge-board seating and QL power. Disconnect power before inspecting internal ribbon or Pico connections |
| Waiting for SD / card will not mount | Check seating and a supported filesystem; try a backed-up, freshly prepared card |
| No images listed | Extract archives, check the `.mdv` extension and hidden attributes, and open the correct folder |
| A file is missing / `...` at the end | Split a large folder into smaller ones; keep names within the browser limits |
| Bad format | Check image format and exact file size; recover a clean copy if necessary |
| Load failed | Check the original filename/path and card readability; a truncated long name can cause this |
| QL cannot read the image | Wait for mounting, use the correct `mdv` number and try a known-good image; inspect wiring only with power disconnected |
| Buttons do not respond | Wait for QL drive activity to finish; normal cartridge controls are gated while selected |
| Eject first | Save with K3, eject with K1/K2, then open Tools |
| Different card / Save blocked | Reinsert the original card and retry K3 without ejecting the image |
| Save failed / Retry save! | Keep power on and the image mounted, resolve card access/free-space problems and retry; the on-card image may already be incomplete |
| Event lost | The RAM image is untrusted. Only K1 eject works; saving is blocked. Reload a known-good saved image |
| Settings did not persist | Leave Tools through Exit or K4 before power-off; settings cannot be saved while the drive is in use |
| SD Check absent | Fit a readable card, leave Tools and reopen it |
| No vibration | Check Motor and the individual event settings; try LED & Motor Test, then inspect optional hardware with power disconnected |
| BLE options absent | Confirm Pico 2 W firmware and eject the mounted image; Lite has no Bluetooth |
| Pairing fails after forgetting a client | Remove the stale pairing on the client, then pair again through Pair Device |

One known quirk: a mainboard powered up on the bench, outside a QL, can pick
up a false drive-select signal and leave the activity LED stuck on — which
also blocks file operations and settings saves. Power-cycle the board and try
again. Inside a QL this doesn't happen; it is purely a bench condition.

## Addendum: creating images with Sinclair MDV Builder

[Sinclair MDV Builder](https://github.com/arleybls/sinclair-mdv-builder) is
this project's free Windows companion app for creating, inspecting, editing
and extracting files from `.mdv` cartridge images. It needs the .NET 8
Desktop Runtime. The basics at a glance:

| What you want | Where it is |
|---|---|
| A new, empty cartridge | New empty cartridge — creates a valid 174,930-byte image |
| A cartridge from files you have | New cartridge from loose files, or drag and drop them in |
| A cartridge from a downloaded ZIP | New cartridge from a ZIP archive |
| Add files to an existing image | Open the `.mdv`, then Import file(s) or Import from ZIP |
| Get files out of an image | Extract a file, Extract All, or Export to ZIP |
| Look inside an image | Directory listing, per-file hex/text viewer, sector map, media info |
| Housekeeping | Duplicate, Rename, Delete, and Set Exec on any file |

Points that matter for this device:

- QL file type and data-space survive ZIP export/import (qlzip-compatible),
  so executables keep their headers.
- Save the finished `.mdv`, then copy it to the SD card as described in
  [Preparing the microSD card](#preparing-the-microsd-card).
- Its images round-trip the builder's own loader and tests; validation on a
  stock QL ROM is still in progress, so keep a backup of anything important.

## Addendum: the Bluetooth protocol for developers

On the Pico 2 W, the Connect mode in System Tools speaks a documented
Bluetooth Low Energy protocol, so you can write your own tool — an app or a
script — to manage the SD card wirelessly. The full wire protocol lives in
[BLE_PROTOCOL.md](BLE_PROTOCOL.md) in this repository. What it lets a client
do, at a glance:

| Area | Operations |
|---|---|
| Files | list folders, upload and download images (SHA-256 verified), rename, delete |
| Cartridge labels | read and rewrite the medium name inside an `.mdv` image |
| Device | info and health snapshots, identify (flash the screen), set the device name |
| Firmware | hand the device over to its wireless update mode |

The essentials: pair once per computer while the device shows **Pair
Device** (a 6-digit passkey appears on the device display), then talk to it
whenever it sits on the **Connect** screen. Uploads are refused while a
cartridge image is mounted, and every transfer is checksummed — a committed
upload is a verified upload. The repository also ships `tools/mdvtool.py`,
a working command line client that doubles as example code.

## Documentation basis

Operation and menu details were checked against
[UserInterface.c](../src/ui/UserInterface.c),
[UserInterfaceExtension.c](../src/ui/UserInterfaceExtension.c),
[SD Check](../src/storage/sd_check.c),
[System Info](../src/ui/sys_info.c),
[filesystem configuration](../src/lib-sdcard/src/include/ffconf.h), and
[BLE implementation](../src/ble/ota_ble.c).
