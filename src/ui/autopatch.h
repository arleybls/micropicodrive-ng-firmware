// Auto-patch: make a mounted cartridge run from the drive this unit is.
//
// QL software hardcodes drive names ("EXEC_W mdv1_game"), so a cartridge
// written for drive 1 fails from drive 2. With the option on, every mdv1_..
// mdv8_ in the files of a freshly mounted image is rewritten, IN RAM ONLY, to
// this unit's drive number (ql_probe_drive()). The same job tools/mdvpatch.py
// does to a file.
//
// The image on the SD card is never changed by this: autopatch_undo() puts the
// original names back before a save and autopatch_redo() re-applies them after.
#ifndef AUTOPATCH_H
#define AUTOPATCH_H

#include <stdbool.h>
#include <stdint.h>

bool autopatch_enabled(void);
void autopatch_enable(bool on);

// Right after an image is loaded into cartridge_image, before it is handed to
// the drive. Does nothing when the option is off or the drive number unknown.
// Returns the number of names changed.
int  autopatch_apply(void);

// For the mounted-cartridge screen: how many names the last apply changed, and
// the digit they were changed to.
int  autopatch_count(void);
char autopatch_drive_char(void);

// The QL wrote this sector: forget the patches in it (cheap; safe to call from
// the drive-event handler).
void autopatch_sector_written(uint8_t sector_no);

void autopatch_undo(void);   // before saving the image to SD
void autopatch_redo(void);   // after the save, whatever its result

#endif
