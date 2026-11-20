// Auto-patch — see autopatch.h.
//
// A drive name is "mdv" in either case, a digit 1-8 and an underscore. Only
// the digit changes, so a patch is one byte and nothing in the image moves.
// Each changed byte is remembered (offset + original digit) so a save can put
// the originals back.
//
// The search is per file block, plus the joins between consecutive blocks of
// the same file: a file's blocks are scattered over the tape, so a name can
// straddle two sectors that are nowhere near each other.
#include "pico/stdlib.h"
#include "UserInterface.h"
#include "SharedBuffers.h"
#include "ql_probe.h"
#include "autopatch.h"

#define NAME_LEN        5      // m d v N _
#define DIGIT_POS       3
#define BLOCK_SIZE      512
#define FILE_HDR_SIZE   64     // QDOS file header at the start of block 0
#define MAX_PATCHES     255    // names beyond this are left as they are

static bool s_enabled;         // default off

static struct { uint32_t offset; uint8_t original; } s_patch[MAX_PATCHES];
static int     s_count;
static uint8_t s_drive_char;   // the digit the patches were made to

bool autopatch_enabled(void) { return s_enabled; }
void autopatch_enable(bool on) { s_enabled = on; }

int  autopatch_count(void) { return s_count; }
char autopatch_drive_char(void) { return (char)s_drive_char; }

static SECTOR_t *sector(int n) { return &((SECTOR_t *)cartridge_image)[n]; }

// File numbers that are not files: directory, sector map, free and bad sectors.
static bool is_file(uint8_t file_no) {
    return file_no != 0x00 && file_no != 0xF8 && file_no != 0xFD && file_no != 0xFF;
}

static void fix_data_checksum(SECTOR_t *s) {
    uint16_t sum = 0x0F0F;
    for (int i = 0; i < BLOCK_SIZE; i++) sum += s->Record.Data[i];
    s->Record.DataChecksum = sum;
}

static bool is_name(const uint8_t *p[NAME_LEN]) {
    return (*p[0] | 0x20) == 'm' && (*p[1] | 0x20) == 'd' && (*p[2] | 0x20) == 'v' &&
           *p[3] >= '1' && *p[3] <= '8' && *p[4] == '_';
}

static void patch_digit(uint8_t *digit, SECTOR_t *owner) {
    if (*digit == s_drive_char || s_count >= MAX_PATCHES) return;
    s_patch[s_count].offset = (uint32_t)(digit - cartridge_image);
    s_patch[s_count].original = *digit;
    s_count++;
    *digit = s_drive_char;
    fix_data_checksum(owner);
}

// The sector holding block `block_no` of file `file_no`, or -1.
static int find_block(uint8_t file_no, uint8_t block_no) {
    for (int n = 0; n < CARTRIDGE_SECTOR_COUNT; n++)
        if (sector(n)->Record.HeaderData[0] == file_no &&
            sector(n)->Record.HeaderData[1] == block_no)
            return n;
    return -1;
}

int autopatch_apply(void) {
    s_count = 0;
    uint8_t drive = ql_probe_drive();
    if (!s_enabled || !drive) return 0;
    s_drive_char = (uint8_t)('0' + drive);

    for (int n = 0; n < CARTRIDGE_SECTOR_COUNT; n++) {
        SECTOR_t *s = sector(n);
        uint8_t file_no = s->Record.HeaderData[0], block_no = s->Record.HeaderData[1];
        if (!is_file(file_no)) continue;
        uint8_t *data = s->Record.Data;
        int start = (block_no == 0) ? FILE_HDR_SIZE : 0;   // never the file header

        // Names wholly inside this block.
        for (int i = start; i + NAME_LEN <= BLOCK_SIZE; i++) {
            const uint8_t *p[NAME_LEN] = { data + i, data + i + 1, data + i + 2,
                                           data + i + 3, data + i + 4 };
            if (is_name(p)) patch_digit(data + i + DIGIT_POS, s);
        }

        // Names that start in this block and end in the file's next one.
        if (block_no == 0xFF) continue;
        int next_n = find_block(file_no, (uint8_t)(block_no + 1));
        if (next_n < 0) continue;
        SECTOR_t *next = sector(next_n);
        for (int head = 1; head < NAME_LEN; head++) {      // bytes taken from this block
            int i = BLOCK_SIZE - head;
            if (i < start) continue;
            const uint8_t *p[NAME_LEN];
            for (int k = 0; k < NAME_LEN; k++)
                p[k] = (k < head) ? data + i + k : next->Record.Data + (k - head);
            if (!is_name(p)) continue;
            if (DIGIT_POS < head) patch_digit(data + i + DIGIT_POS, s);
            else                  patch_digit(next->Record.Data + (DIGIT_POS - head), next);
        }
    }
    return s_count;
}

// The QL has just rewritten this sector: what is in it now is the QL's data,
// not a name this module patched, so forget every patch inside it. Without
// this, undo could "restore" a digit into data the QL wrote there since.
#define PATCH_DEAD UINT32_MAX
void autopatch_sector_written(uint8_t sector_no) {
    uint32_t lo = (uint32_t)sector_no * CARTRIDGE_SECTOR_SIZE;
    for (int i = 0; i < s_count; i++)
        if (s_patch[i].offset >= lo && s_patch[i].offset < lo + CARTRIDGE_SECTOR_SIZE)
            s_patch[i].offset = PATCH_DEAD;
}

// Swap the remembered bytes between "original" and "patched".
static void swap_all(bool to_original) {
    for (int i = 0; i < s_count; i++) {
        if (s_patch[i].offset == PATCH_DEAD) continue;
        uint8_t *digit = cartridge_image + s_patch[i].offset;
        uint8_t from = to_original ? s_drive_char : s_patch[i].original;
        uint8_t to   = to_original ? s_patch[i].original : s_drive_char;
        if (*digit != from) continue;
        *digit = to;
        fix_data_checksum(sector((int)(s_patch[i].offset / CARTRIDGE_SECTOR_SIZE)));
    }
}

void autopatch_undo(void) { swap_all(true); }
void autopatch_redo(void) { swap_all(false); }
