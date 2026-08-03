// BLE OTA firmware update — device side. See PLAN_OTA_UPDATE.md.
//
// [MicroPicoDrive adaptation of the UIExt sandbox ota_ble.c, commit 057b1ae.
//  Local change, everything else verbatim: §5b SD mutations and the reboot
//  op answer EBUSY while a cartridge is mounted / the QL is using the drive
//  (cartridge_busy() in UserInterface.c) — BLE_PROTOCOL.md mounted-cartridge
//  rule. A watchdog reset is electrically harmless to the QL (GPIOs high-Z)
//  but would fail an in-flight LOAD/SAVE.]
//
// v1 scope notes (documented deviations from the full plan):
//  - No TBYB yet: images are stamped with an incrementing version (picotool
//    seal) and the bootrom boots the newest valid slot. Slot header (sector 0)
//    is written LAST, after user confirmation, so an interrupted or declined
//    transfer never leaves a bootable half-image. Rollback = Revert FW menu
//    (erases the active header) or USB BOOTSEL.
//  - No image signature yet: integrity is SHA-256 (hardware) against the
//    manifest; authenticity comes from the passkey-bonded encrypted link.
//
// Concurrency model: BTstack callbacks run on the cyw43 async context
// (threadsafe_background). The data path is a single-producer (BT context)
// single-consumer (UI loop) ring buffer with monotonic indices; all other
// BTstack calls from the UI loop are wrapped in the async-context lock.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "pico/flash.h"
#include "pico/sha256.h"
#include "pico/bootrom.h"
#include "boot/picobin.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"   // "health": on-die temperature sensor
#include <malloc.h>         // "health": free-heap report
#include "btstack.h"
#include "btstack_tlv.h"  // peer-name cache (shares the bond-storage TLV)
#include "ota.h"          // generated GATT database (profile_data + handles)
#include "ota_ble.h"
#include "sd_menu.h"      // shared FatFs mount + ff.h (Connect-mode SD serve)
#include "UserInterfaceExtension.h"
#define JSMN_STATIC       // file-local jsmn symbols
#include "jsmn.h"

// ── Protocol constants (a BLE upload client must mirror these) ───────────────
#define OTA_OP_START      0x01  // [1][size u32][major u16][minor u16][sha256 32B]
#define OTA_OP_ABORT      0x02

#define OTA_ST_READY      0x01  // [1][window u32]
#define OTA_ST_ACK        0x02  // [1][next_expected_offset u32]
#define OTA_ST_ERROR      0x03  // [1][code u8]
#define OTA_ST_VERIFIED   0x04
#define OTA_ST_COMMITTED  0x05
#define OTA_ST_DECLINED   0x06

#define OTA_ERR_SIZE      1
#define OTA_ERR_VERSION   2
#define OTA_ERR_STATE     3
#define OTA_ERR_HASH      5
#define OTA_ERR_FLASH     6
#define OTA_ERR_CANCEL    7
#define OTA_ERR_TIMEOUT   8

#define OTA_RING_SIZE     8192          // credit window advertised in READY
#define OTA_IDLE_TIMEOUT_MS   120000    // advertising / waiting for START
#define OTA_STALL_TIMEOUT_MS  30000     // mid-transfer silence
// Shared confirm timeout (UX review X12: dialogs used to differ invisibly).
#define OTA_CONFIRM_TIMEOUT_MS UIEXT_CONFIRM_TIMEOUT_MS

// ── ATT handles from the generated database ──────────────────────────────────
#define H_INFO    ATT_CHARACTERISTIC_7A0B0002_D5A9_4B7C_9F2A_5B1E6F0C4D10_01_VALUE_HANDLE
#define H_CONTROL ATT_CHARACTERISTIC_7A0B0003_D5A9_4B7C_9F2A_5B1E6F0C4D10_01_VALUE_HANDLE
#define H_DATA    ATT_CHARACTERISTIC_7A0B0004_D5A9_4B7C_9F2A_5B1E6F0C4D10_01_VALUE_HANDLE
#define H_STATUS  ATT_CHARACTERISTIC_7A0B0005_D5A9_4B7C_9F2A_5B1E6F0C4D10_01_VALUE_HANDLE
#define H_STATUS_CCC ATT_CHARACTERISTIC_7A0B0005_D5A9_4B7C_9F2A_5B1E6F0C4D10_01_CLIENT_CONFIGURATION_HANDLE

// ── Module state ─────────────────────────────────────────────────────────────
typedef enum { OTA_IDLE, OTA_ADVERTISING, OTA_CONNECTED, OTA_RECEIVING,
               OTA_DONE_OK, OTA_DONE_ERR } ota_state_t;

static bool     s_ble_inited;            // btstack initialised once per power-on
static bool     s_pairing_allowed;       // true only in Pair Device mode

// ── Connect mode (JSON command server over the same GATT — BLE_PROTOCOL.md §5) ─
// In Connect mode the CONTROL char carries JSON commands (not OTA opcodes), the
// INFO char returns JSON device info, and STATUS carries §6 response frames.
static volatile bool s_connect_mode;     // true inside ota_run_connect_mode()
static volatile bool s_cmd_ready;        // a JSON command is waiting in s_cmd_buf
static uint8_t  s_cmd_buf[256];          // last CONTROL write (JSON), BT context
static volatile uint16_t s_cmd_len;
static char     s_info_json[256];        // prebuilt INFO JSON, refreshed on entry
static int      s_info_len;
static volatile bool     s_xfer_active;   // a file transfer is in progress
static volatile bool     s_xfer_send;     // true = sending (read), false = receiving
static volatile int      s_xfer_pct;      // 0..100
static volatile uint32_t s_xfer_ms;       // ms of the last transfer activity
static char              s_xfer_name[32];  // basename shown on the transfer screen
static volatile ota_state_t s_state;
static volatile hci_con_handle_t s_con_handle = HCI_CON_HANDLE_INVALID;
static volatile bool s_status_notify_on;
static volatile int s_slots_total;          // max free LE ACL slots seen this connection

// §5 heartbeat: armed by the first "ping"; any inbound CMD/DATAIN write counts
// as liveness. 3 missed pings (app cadence ~10 s) = peer presumed dead.
#define UIEXT_HB_TIMEOUT_MS 30000
static volatile uint32_t s_last_inbound_ms;
static volatile bool s_hb_armed;

// §5 Pico Tools: persisted device name (TLV, same store as peer names) and the
// identify-flash deadline (set by process_command, drawn by the connect loop).
#define DEV_NAME_MAX 16
#define DEV_NAME_TAG 0x444E0000u   // 'D','N',0,0
static uint32_t s_identify_until;  // != 0 → alternate white/normal until this tick
static volatile bool s_encrypted;
static volatile uint32_t s_passkey;      // 0 = none pending
static volatile bool s_passkey_pending;
static volatile bool s_pairing_done;
static volatile uint8_t s_pairing_status;
static volatile uint8_t s_err_code;

// Transfer state (written by BT callback, consumed by UI loop)
static volatile uint32_t s_total_size;
static volatile uint16_t s_new_major, s_new_minor;
static uint8_t  s_expect_sha[32];
static volatile bool s_start_received;
static volatile bool s_abort_received;

// SPSC ring: producer = BT context (att write), consumer = UI loop
static uint8_t  s_ring[OTA_RING_SIZE];
static volatile uint32_t s_ring_head;    // bytes ever accepted (producer)
static volatile uint32_t s_ring_tail;    // bytes ever consumed (consumer)

// Flash geometry (physical offsets, from the partition table)
static uint32_t s_slot_off[2], s_slot_size[2];
static int      s_active_slot = -1;      // -1 = not a partition boot
static bool     s_pt_queried;

static char     s_version_str[16];
static char     s_devname[17];           // "MPD-XXXX" default; user setname ≤16 (§5)

// Primary service UUID 7A0B0001-D5A9-4B7C-9F2A-5B1E6F0C4D10 in little-endian
// advertising byte order. The app scans filtered by this UUID (§1), so it MUST
// ride in the advertisement — the display name goes in the scan response.
static const uint8_t s_service_uuid128[16] = {
    0x10, 0x4d, 0x0c, 0x6f, 0x1e, 0x5b, 0x2a, 0x9f,
    0x7c, 0x4b, 0xa9, 0xd5, 0x01, 0x00, 0x0b, 0x7a
};

// ── Small helpers ────────────────────────────────────────────────────────────
const char *ota_fw_version_string(void) {
    if (!s_version_str[0])
        snprintf(s_version_str, sizeof(s_version_str), "v%d.%d.%d",
                 UIEXT_FW_VERSION_MAJOR, UIEXT_FW_VERSION_MINOR,
                 UIEXT_FW_VERSION_PATCH);
    return s_version_str;
}

// The BLE wire and the sealed IMAGE_DEF carry the COMBINED minor*100+patch
// ordering value (see ota_ble.h) — decode it for display, or "v2.5.13"
// renders as "v2.513".
static void fmt_combined_version(char *dst, size_t n, const char *prefix,
                                 unsigned maj, unsigned combined) {
    snprintf(dst, n, "%sv%u.%u.%u", prefix, maj, combined / 100u, combined % 100u);
}

// Park a fully received+verified OTA image in SD:/.update as a bin+manifest
// pair (build_ota.ps1 format) so the boot-time SD update can offer it again —
// used when the install confirm TIMES OUT (an explicit No still discards).
// Sector 0 comes from the held-back RAM copy; the rest is read back from the
// inactive slot via the physical (untranslated) flash view.
static bool ota_park_update(uint32_t t_off, const uint8_t *sec0, uint32_t sec0_len) {
    if (s_total_size == 0 || sec0_len == 0 || sec0_len > s_total_size) return false;
    if (!sd_menu_available()) return false;
    f_mkdir("/.update");   // no-op when it already exists

    unsigned min = s_new_minor / 100u, pat = s_new_minor % 100u;
    char bin[56], json[56];
    snprintf(bin, sizeof(bin), "/.update/MicroPicoDrive_v%u.%u.%u.bin",
             s_new_major, min, pat);
    snprintf(json, sizeof(json), "/.update/MicroPicoDrive_v%u.%u.%u.json",
             s_new_major, min, pat);

    FIL f;
    UINT bw = 0;
    if (f_open(&f, bin, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    bool ok = f_write(&f, sec0, sec0_len, &bw) == FR_OK && bw == sec0_len;
    if (ok && s_total_size > sec0_len) {
        const uint8_t *img = (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
                                               + t_off + sec0_len);
        UINT rest = (UINT)(s_total_size - sec0_len);
        ok = f_write(&f, img, rest, &bw) == FR_OK && bw == rest;
    }
    ok = (f_close(&f) == FR_OK) && ok;

    if (ok) {
        char hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(&hex[i * 2], 3, "%02x", s_expect_sha[i]);
        char txt[192];
        int n = snprintf(txt, sizeof(txt),
                         "{\"major\":%u,\"minor\":%u,\"patch\":%u,\"size\":%u,\"sha256\":\"%s\"}",
                         (unsigned)s_new_major, min, pat,
                         (unsigned)s_total_size, hex);
        ok = f_open(&f, json, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
        if (ok) {
            ok = f_write(&f, txt, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
            ok = (f_close(&f) == FR_OK) && ok;
        }
    }
    if (!ok) { f_unlink(bin); f_unlink(json); }   // never leave a half pair
    printf("[ota] confirm timeout: park to /.update %s\n", ok ? "ok" : "FAILED");
    return ok;
}

static void ota_query_partitions(void) {
    if (s_pt_queried) return;
    s_pt_queried = true;
    boot_info_t bi = {0};
    if (rom_get_boot_info(&bi) <= 0) { s_active_slot = -1; return; }
    s_active_slot = (bi.partition == 0 || bi.partition == 1) ? bi.partition : -1;
    if (s_active_slot < 0) return;
    for (int p = 0; p < 2; p++) {
        uint32_t buf[4] = {0};
        int ret = rom_get_partition_table_info(buf, count_of(buf),
                 PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | ((uint32_t)p << 24));
        if (ret != 3) { s_active_slot = -1; return; }
        uint32_t loc = buf[1];
        uint32_t first = (loc >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB) & 0x1fffu;
        uint32_t last  = (loc >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB)  & 0x1fffu;
        s_slot_off[p]  = first * FLASH_SECTOR_SIZE;
        s_slot_size[p] = (last - first + 1) * FLASH_SECTOR_SIZE;
    }
}

bool ota_partition_boot(void) {
    ota_query_partitions();
    return s_active_slot >= 0;
}

// Slot/flash primitives for the SD-card update path (sd_update_ota.c)
bool ota_slots(uint32_t *active_off, uint32_t *target_off, uint32_t *target_size) {
    ota_query_partitions();
    if (s_active_slot < 0) return false;
    int t = 1 - s_active_slot;
    if (active_off)  *active_off  = s_slot_off[s_active_slot];
    if (target_off)  *target_off  = s_slot_off[t];
    if (target_size) *target_size = s_slot_size[t];
    return true;
}

static bool flash_write_sector(uint32_t off, const uint8_t *data, uint32_t len);
static bool flash_erase_sector(uint32_t off);
bool ota_flash_sector(uint32_t flash_off, const uint8_t *data, uint32_t len) {
    return flash_write_sector(flash_off, data, len);
}
bool ota_erase_sector_at(uint32_t flash_off) {
    return flash_erase_sector(flash_off);
}

bool ota_other_slot_has_image(void) {
    ota_query_partitions();
    if (s_active_slot < 0) return false;
    int other = 1 - s_active_slot;
    // Physical (untranslated) flash view; erased header = no image.
    const uint32_t *w = (const uint32_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + s_slot_off[other]);
    return *w != 0xFFFFFFFFu;
}

// Read the sealed image version (picotool seal --major/--minor) from a slot.
// A sealed image carries a picobin block LOOP: the crt0 IMAGE_DEF in the first
// 4K (usually without a version) links to the block picotool appends at the
// image end, which holds the VERSION item (payload word = major<<16 | minor).
// Walk the loop, following each block's relative link word.
static bool ota_slot_version(int slot, uint16_t *major, uint16_t *minor) {
    if (slot < 0) return false;
    const uint32_t *base = (const uint32_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + s_slot_off[slot]);
    const uint32_t slot_words = s_slot_size[slot] / 4;
    // First block marker (bootrom requires it in the first 4K).
    uint32_t blk = UINT32_MAX;
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE / 4 && i < slot_words; i++) {
        if (base[i] == PICOBIN_BLOCK_MARKER_START) { blk = i; break; }
    }
    for (int hops = 0; hops < 4 && blk != UINT32_MAX; hops++) {
        uint32_t j = blk + 1;
        uint32_t items_total = 0;   // words, from the LAST item's size field
        while (j < slot_words) {
            uint32_t w = base[j];
            uint8_t type = (uint8_t)(w & 0xFF);
            // Item size in words: 1-byte size in bits 15:8, or 16-bit size for
            // 0x80-flagged (2BS) types in bits 23:8.
            uint32_t size = (type & 0x80) ? ((w >> 8) & 0xFFFF) : ((w >> 8) & 0xFF);
            if (size == 0) return false;
            if (type == PICOBIN_BLOCK_ITEM_1BS_VERSION && j + 1 < slot_words) {
                uint32_t v = base[j + 1];
                *major = (uint16_t)(v >> 16);
                *minor = (uint16_t)(v & 0xFFFF);
                return true;
            }
            if (type == PICOBIN_BLOCK_ITEM_2BS_LAST) { items_total = size; break; }
            j += size;
        }
        if (!items_total) return false;
        // After the items: relative byte offset from this block's marker to the
        // next block's marker; 0 = no further blocks. The LAST item's size
        // field counts the items EXCLUDING itself (verified against real
        // sealed images), so the link sits one word past it.
        uint32_t link_idx = blk + 1 + items_total + 1;
        if (link_idx >= slot_words) return false;
        int32_t rel = (int32_t)base[link_idx];
        if (rel == 0) return false;
        int64_t next = (int64_t)blk + rel / 4;
        if (next <= 0 || (uint64_t)next >= slot_words ||
            base[next] != PICOBIN_BLOCK_MARKER_START) return false;
        blk = (uint32_t)next;
    }
    return false;
}

// ── Flash ops via flash_safe_execute (physical offsets) ──────────────────────
typedef struct { uint32_t off; const uint8_t *data; uint32_t len; } flash_job_t;

static void flash_erase_program_cb(void *param) {
    const flash_job_t *j = (const flash_job_t *)param;
    flash_range_erase(j->off, FLASH_SECTOR_SIZE);
    if (j->data) flash_range_program(j->off, j->data, j->len);
}

static bool flash_write_sector(uint32_t off, const uint8_t *data, uint32_t len) {
    // len <= FLASH_SECTOR_SIZE; pad program length to 256-byte pages
    static uint8_t page_buf[FLASH_SECTOR_SIZE];
    uint32_t plen = (len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1u);
    memset(page_buf, 0xFF, plen);
    memcpy(page_buf, data, len);
    flash_job_t j = { .off = off, .data = page_buf, .len = plen };
    return flash_safe_execute(flash_erase_program_cb, &j, 2000) == PICO_OK;
}

static bool flash_erase_sector(uint32_t off) {
    flash_job_t j = { .off = off, .data = NULL, .len = 0 };
    return flash_safe_execute(flash_erase_program_cb, &j, 2000) == PICO_OK;
}

// ── BTstack plumbing ─────────────────────────────────────────────────────────
static btstack_packet_callback_registration_t s_hci_reg;
static btstack_packet_callback_registration_t s_sm_reg;

static void ble_lock(void)   { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
static void ble_unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }

// ── Peer-name capture ────────────────────────────────────────────────────────
// Once a link encrypts (Connect/Pair mode) the firmware GATT-reads the
// central's GAP Device Name (0x2A00) and caches it in the BTstack TLV keyed by
// the peer's bond slot, so the Paired Devices list can show names, not MACs.
#define PEER_NAME_MAX 24
#define PEER_NAME_TAG(slot) (0x504E0000u | (uint32_t)(slot))   // 'P','N',0,slot

static volatile bool s_name_read_started;   // one read per connection
static volatile bool s_name_pending;        // BT ctx stashed a name for the UI loop
static char s_peer_name[PEER_NAME_MAX];     // stash (BT ctx writes, UI loop reads)

// TLV helpers — caller must hold ble_lock (TLV is shared with bond storage).
static int peer_name_get(int slot, char *out, int outsz) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv) return 0;
    int n = tlv->get_tag(ctx, PEER_NAME_TAG(slot), (uint8_t *)out, (uint32_t)(outsz - 1));
    if (n <= 0) return 0;
    if (n > outsz - 1) n = outsz - 1;
    out[n] = '\0';
    return n;
}

static void peer_name_delete(int slot) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv) tlv->delete_tag(ctx, PEER_NAME_TAG(slot));
}

// GATT client events run in BT context: stash only, no flash writes here.
static void gatt_name_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT) return;
    uint16_t len = gatt_event_characteristic_value_query_result_get_value_length(packet);
    if (len == 0) return;
    if (len > PEER_NAME_MAX - 1) len = PEER_NAME_MAX - 1;
    memcpy(s_peer_name, gatt_event_characteristic_value_query_result_get_value(packet), len);
    s_peer_name[len] = '\0';
    s_name_pending = true;
}

// Kick off the read once encrypted, then commit the result to the TLV — called
// regularly from the Connect/Pair mode loops (UI loop, flash writes allowed).
static void peer_name_poll(void) {
    if (s_encrypted && s_con_handle != HCI_CON_HANDLE_INVALID && !s_name_read_started) {
        s_name_read_started = true;
        ble_lock();
        gatt_client_read_value_of_characteristics_by_uuid16(
            gatt_name_handler, s_con_handle, 0x0001, 0xffff, 0x2a00);
        ble_unlock();
    }
    if (s_name_pending) {
        s_name_pending = false;
        ble_lock();
        int slot = sm_le_device_index(s_con_handle);
        if (slot >= 0) {
            const btstack_tlv_t *tlv = NULL;
            void *ctx = NULL;
            btstack_tlv_get_instance(&tlv, &ctx);
            if (tlv) tlv->store_tag(ctx, PEER_NAME_TAG(slot),
                                    (const uint8_t *)s_peer_name,
                                    (uint32_t)strlen(s_peer_name));
        }
        ble_unlock();
        if (slot >= 0) printf("[ble] peer name '%s' (slot %d)\n", s_peer_name, slot);
    }
}

static void status_notify(const uint8_t *data, uint16_t len) {
    if (s_con_handle == HCI_CON_HANDLE_INVALID || !s_status_notify_on) return;
    ble_lock();
    att_server_notify(s_con_handle, H_STATUS, data, len);
    ble_unlock();
}

static void notify_ack(void) {
    uint8_t msg[5] = { OTA_ST_ACK };
    uint32_t off = s_ring_head;   // bytes accepted so far
    memcpy(&msg[1], &off, 4);
    status_notify(msg, sizeof(msg));
}

static void notify_simple(uint8_t op) { status_notify(&op, 1); }

static void notify_error(uint8_t code) {
    uint8_t msg[2] = { OTA_ST_ERROR, code };
    status_notify(msg, sizeof(msg));
}

// ── §5b push session (phone → unit SD) ───────────────────────────────────────
// wbegin creates "<path>.part"; DATAIN frames stream through the OTA ring
// (BT ctx producer / UI loop consumer, record = [flags][seq16][len16][bytes]);
// the UI loop appends to the file, sends wack {nextOffset} every 16 KB (and on
// gap/idle), and wcommit verifies SHA-256 then renames atomically over path.
#define W_ACK_WINDOW   16384
#define W_GAP_IDLE_MS  150
#define W_SESSION_TIMEOUT_MS 30000

static volatile bool s_w_active;         // session open (producer gate)
static volatile uint8_t s_w_reqid;       // wbegin reqId — frames must match
static volatile bool s_w_disc;           // disconnected mid-push: cleanup in UI loop
static FIL      s_w_fil;                 // open "<path>.part"
static char     s_w_path[96], s_w_tmp[104];
static char     s_w_sha[65];             // expected content sha256 (hex)
static uint32_t s_w_size, s_w_received, s_w_acked;
static int32_t  s_w_expected;            // next seq; -1 = adopt first seen (resync)
static volatile bool s_w_gap;            // seq gap: discard until resync wack
static bool     s_w_last_seen;           // frame with LAST processed in-sequence
static uint32_t s_w_last_rx_ms;          // last consumed record (idle ack + timeout)

// ATT read: device info characteristic
static uint16_t att_read_cb(hci_con_handle_t con, uint16_t handle,
                            uint16_t offset, uint8_t *buffer, uint16_t size) {
    (void)con;
    if (handle == H_INFO && s_connect_mode) {
        // JSON device info for the companion app (§5). Prebuilt on mode entry.
        if (buffer) {
            if (offset > (uint16_t)s_info_len) return 0;
            uint16_t c = (uint16_t)s_info_len - offset;
            if (c > size) c = size;
            memcpy(buffer, s_info_json + offset, c);
            return c;
        }
        return (uint16_t)s_info_len;
    }
    if (handle == H_INFO) {
        char info[64];
        int n = snprintf(info, sizeof(info), "%s;slot%c;max=%lu;proto=1",
                         ota_fw_version_string(),
                         s_active_slot == 0 ? 'A' : 'B',
                         (unsigned long)(s_active_slot >= 0 ? s_slot_size[1 - s_active_slot] : 0));
        if (n < 0) n = 0;
        if (buffer) {
            if (offset > (uint16_t)n) return 0;
            uint16_t c = (uint16_t)n - offset;
            if (c > size) c = size;
            memcpy(buffer, info + offset, c);
            return c;
        }
        return (uint16_t)n;
    }
    return 0;
}

// ATT write: control + data + CCC (runs in BT context)
static int att_write_cb(hci_con_handle_t con, uint16_t handle,
                        uint16_t mode, uint16_t offset,
                        uint8_t *buffer, uint16_t size) {
    (void)mode; (void)offset;
    if (handle == H_STATUS_CCC) {
        s_status_notify_on = size >= 2 && (buffer[0] & 1);
        return 0;
    }
    if (handle == H_CONTROL && s_connect_mode) {
        // JSON command — copy out fast and let the UI loop parse/serve it.
        s_last_inbound_ms = to_ms_since_boot(get_absolute_time());
        uint16_t n = size < sizeof(s_cmd_buf) ? size : (uint16_t)sizeof(s_cmd_buf);
        memcpy(s_cmd_buf, buffer, n);
        s_cmd_len = n;
        s_cmd_ready = true;
        return 0;
    }
    if (handle == H_DATA && s_connect_mode) {
        s_last_inbound_ms = to_ms_since_boot(get_absolute_time());
        // §5b push frame → ring record [flags][seq16][len16][payload]. A full
        // ring drops the frame; the seq gap triggers the wack resume path.
        if (!s_w_active || size < 4 || buffer[0] != s_w_reqid) return 0;
        uint16_t plen = (uint16_t)(size - 4);
        uint32_t need = 5u + plen;
        if (s_ring_head - s_ring_tail + need > OTA_RING_SIZE) return 0;
        uint8_t hdr[5] = { buffer[1], buffer[2], buffer[3],
                           (uint8_t)plen, (uint8_t)(plen >> 8) };
        uint32_t head = s_ring_head;
        for (uint32_t i = 0; i < 5; i++) s_ring[(head + i) % OTA_RING_SIZE] = hdr[i];
        head += 5;
        uint32_t pos = head % OTA_RING_SIZE;
        uint32_t first = OTA_RING_SIZE - pos;
        if (first > plen) first = plen;
        memcpy(&s_ring[pos], buffer + 4, first);
        if (plen > first) memcpy(&s_ring[0], buffer + 4 + first, plen - first);
        s_ring_head = head + plen;   // single head update = record is atomic
        return 0;
    }
    if (handle == H_CONTROL) {
        if (size >= 1 && buffer[0] == OTA_OP_START && size >= 41 && s_state == OTA_CONNECTED) {
            memcpy((void *)&s_total_size, &buffer[1], 4);
            memcpy((void *)&s_new_major, &buffer[5], 2);
            memcpy((void *)&s_new_minor, &buffer[7], 2);
            memcpy(s_expect_sha, &buffer[9], 32);
            s_start_received = true;
        } else if (size >= 1 && buffer[0] == OTA_OP_ABORT) {
            s_abort_received = true;
        }
        return 0;
    }
    if (handle == H_DATA && s_state == OTA_RECEIVING) {
        if (size < 5) return 0;
        uint32_t off;
        memcpy(&off, buffer, 4);
        uint32_t len = size - 4;
        if (off + len <= s_ring_head) return 0;              // duplicate, ignore
        if (off != s_ring_head) return 0;                    // gap: wait for ACK-driven rewind
        if (s_ring_head - s_ring_tail + len > OTA_RING_SIZE) return 0;  // no space: client overran window
        uint32_t pos = s_ring_head % OTA_RING_SIZE;
        uint32_t first = OTA_RING_SIZE - pos;
        if (first > len) first = len;
        memcpy(&s_ring[pos], buffer + 4, first);
        if (len > first) memcpy(&s_ring[0], buffer + 4 + first, len - first);
        s_ring_head += len;
        (void)con;
        return 0;
    }
    return 0;
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t ev = hci_event_packet_get_type(packet);
    switch (ev) {
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                s_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                s_slots_total = 0;   // re-learn the slot ceiling per connection
                s_hb_armed = false;  // heartbeat re-arms on this session's first ping
                s_last_inbound_ms = to_ms_since_boot(get_absolute_time());
                if (s_state == OTA_ADVERTISING) s_state = OTA_CONNECTED;
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            s_con_handle = HCI_CON_HANDLE_INVALID;
            s_status_notify_on = false;
            s_encrypted = false;
            s_name_read_started = false;   // next connection re-reads the name
            if (s_w_active) s_w_disc = true;   // abort the push in the UI loop
            if (s_state == OTA_CONNECTED || s_state == OTA_RECEIVING) s_state = OTA_ADVERTISING;
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            s_encrypted = hci_event_encryption_change_get_encryption_enabled(packet) != 0;
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            // Unauthenticated pairing is never acceptable (no MITM protection).
            sm_bonding_decline(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            if (s_pairing_allowed) {
                s_passkey = sm_event_passkey_display_number_get_passkey(packet);
                s_passkey_pending = true;
            } else {
                sm_bonding_decline(sm_event_passkey_display_number_get_handle(packet));
            }
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            s_pairing_status = sm_event_pairing_complete_get_status(packet);
            s_pairing_done = true;
            break;
        default:
            break;
    }
}

static void ble_build_adv(uint8_t *adv, uint8_t *adv_len) {
    // Flags + complete list of 128-bit service UUIDs. The app filters its scan
    // by the service UUID (never by name — BLE_PROTOCOL.md §1), so the UUID must
    // be here in the advertisement; the name rides in the scan response below.
    uint8_t i = 0;
    adv[i++] = 2; adv[i++] = BLUETOOTH_DATA_TYPE_FLAGS; adv[i++] = 0x06;
    adv[i++] = 17; adv[i++] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS;
    memcpy(&adv[i], s_service_uuid128, 16); i += 16;
    *adv_len = i;   // 21 bytes
}

static void ble_build_scan_rsp(uint8_t *rsp, uint8_t *rsp_len) {
    // Complete local name "MPD-XXXX" — display only (§1).
    uint8_t n = (uint8_t)strlen(s_devname);
    uint8_t i = 0;
    rsp[i++] = (uint8_t)(n + 1); rsp[i++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&rsp[i], s_devname, n); i += n;
    *rsp_len = i;
}

static bool s_quiet;   // suppress radio-init screens (quiet stack init, e.g. paired list)

static void trace(const char *l2) {
    if (!s_quiet) uiext_ota_status("Starting Radio", l2);
}

static bool ble_stack_init(void) {
    if (!s_ble_inited) {
        trace("(cyw43 init)");
        printf("[ota] cyw43_arch_init...\n");
        int rc = cyw43_arch_init();
        printf("[ota] cyw43_arch_init rc=%d\n", rc);
        if (rc) {
            if (!s_quiet) {
                char e[24];
                snprintf(e, sizeof(e), "cyw43 err %d", rc);
                uiext_ota_status("Radio Init Failed", e);
                sleep_ms(2500);
            }
            return false;
        }
        trace("(BT stack)");
        printf("[ota] btstack setup...\n");
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        snprintf(s_devname, sizeof(s_devname), "MPD-%02X%02X",
                 id.id[6], id.id[7]);
        {   // §5 setname: persisted name overrides the default (TLV is already
            // set up — btstack_cyw43_init ran inside cyw43_arch_init above).
            const btstack_tlv_t *tlv = NULL;
            void *ctx = NULL;
            btstack_tlv_get_instance(&tlv, &ctx);
            char nm[DEV_NAME_MAX + 1];
            int n = tlv ? tlv->get_tag(ctx, DEV_NAME_TAG, (uint8_t *)nm, DEV_NAME_MAX) : 0;
            if (n >= 1 && n <= DEV_NAME_MAX) {
                nm[n] = 0;
                memcpy(s_devname, nm, (size_t)n + 1);
            }
        }
        l2cap_init();
        sm_init();
        gatt_client_init();   // peer-name read (GAP Device Name)
        sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
        sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION |
                                           SM_AUTHREQ_MITM_PROTECTION |
                                           SM_AUTHREQ_BONDING);
        // Security Request on every connection: a bonded central re-encrypts
        // immediately, so its CCCD re-subscribe never races lazy re-encryption
        // (the reconnect wedge: notify_on stayed off, every reply was refused).
        sm_set_request_security(true);
        att_server_init(profile_data, att_read_cb, att_write_cb);
        s_hci_reg.callback = &packet_handler;
        hci_add_event_handler(&s_hci_reg);
        s_sm_reg.callback = &sm_packet_handler;
        sm_add_event_handler(&s_sm_reg);
        att_server_register_packet_handler(packet_handler);
        s_ble_inited = true;
        printf("[ota] btstack setup done\n");
    }
    return true;
}

static bool ble_up(void) {
    if (!ble_stack_init()) return false;
    uiext_ota_status("Starting Radio", "(power on)");
    printf("[ota] hci power on\n");
    ble_lock();
    hci_power_control(HCI_POWER_ON);
    ble_unlock();
    // wait for stack to come up (HCI_STATE_WORKING)
    for (int i = 0; i < 200; i++) {
        ble_lock();
        bool up = hci_get_state() == HCI_STATE_WORKING;
        ble_unlock();
        if (up) { printf("[ota] hci working\n"); return true; }
        sleep_ms(50);
    }
    printf("[ota] hci never reached WORKING\n");
    uiext_ota_status("Starting Radio", "Power-on timed out");
    sleep_ms(2500);
    return false;
}

static void ble_advertise(bool on) {
    ble_lock();
    if (on) {
        static uint8_t adv[31];
        static uint8_t rsp[31];
        uint8_t adv_len, rsp_len;
        ble_build_adv(adv, &adv_len);
        ble_build_scan_rsp(rsp, &rsp_len);
        uint16_t iv = 800;  // 500 ms
        bd_addr_t null_addr = {0};
        gap_advertisements_set_params(iv, iv, 0, 0, null_addr, 0x07, 0);
        gap_advertisements_set_data(adv_len, adv);
        gap_scan_response_set_data(rsp_len, rsp);
    }
    gap_advertisements_enable(on ? 1 : 0);
    ble_unlock();
}

static void ble_down(void) {
    ble_advertise(false);
    ble_lock();
    if (s_con_handle != HCI_CON_HANDLE_INVALID)
        gap_disconnect(s_con_handle);
    hci_power_control(HCI_POWER_OFF);
    ble_unlock();
    s_con_handle = HCI_CON_HANDLE_INVALID;
    s_status_notify_on = false;
    s_state = OTA_IDLE;
}

// ── Buttons (active-low) ─────────────────────────────────────────────────────
static bool btn_cancel_pressed(void) {
    if (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0) {
        while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
            sleep_ms(20);
        sleep_ms(150);
        return true;
    }
    return false;
}

// ── Connect mode: JSON command server (browse/pull SD) ───────────────────────
// Everything below runs on the UI loop, never in BT context. The BT callbacks
// only stash the incoming command (att_write_cb) and serve INFO (att_read_cb).

// One notify attempt, holding the async lock (BTstack call convention).
static int notify_once(const uint8_t *d, uint16_t n) {
    ble_lock();
    int rc = att_server_notify(s_con_handle, H_STATUS, d, n);
    ble_unlock();
    return rc;
}

// Send one payload as a notification, waiting out a full ACL buffer.
static bool notify_raw(const uint8_t *d, uint16_t n) {
    if (s_con_handle == HCI_CON_HANDLE_INVALID || !s_status_notify_on) {
        printf("[ntf] refused: handle=%d notify_on=%d\n",
               (int)s_con_handle, (int)s_status_notify_on);
        return false;
    }
    int rc = 0;
    for (int tries = 0; tries < 3000; tries++) {
        // Self-imposed LE flow control: btstack_config.h caps the controller
        // queue at 3 (MAX_NR_CONTROLLER_ACL_BUFFERS, "avoid overrunning the
        // shared CYW43 bus") but BTstack applies that to Classic only — LE runs
        // at the full reported depth (7+), and running the CYW43 that deep is
        // what stalls its completed-packets events (hardware-confirmed wedge).
        // Keep at most 3 of our packets in flight.
        ble_lock();
        int free_slots = hci_number_free_acl_slots_for_handle(s_con_handle);
        ble_unlock();
        if (free_slots > s_slots_total) s_slots_total = free_slots;
        if (free_slots < s_slots_total - 2) {
            sleep_ms(1);
            if (s_con_handle == HCI_CON_HANDLE_INVALID) return false;
            continue;
        }
        rc = notify_once(d, n);
        if (rc == 0) return true;
        if (rc != BTSTACK_ACL_BUFFERS_FULL) {
            // Real error / not connected — reqId+seq identify the frame that died.
            printf("[ntf] rc=%d req=%u seq=%u len=%u\n", rc, d[0],
                   (unsigned)(d[2] | (d[3] << 8)), (unsigned)n);
            return false;
        }
        sleep_ms(1);                                          // let the run loop drain
        if (s_con_handle == HCI_CON_HANDLE_INVALID) return false;
    }
    // 3 s of ACL_BUFFERS_FULL: the CYW43 send path is wedged (see the §6 hard
    // rules in BLE_PROTOCOL.md). Nothing sends again on this link — drop it so
    // both sides reset instead of a zombie "Connected".
    printf("[ntf] STARVED req=%u seq=%u len=%u — dropping link to recover\n",
           d[0], (unsigned)(d[2] | (d[3] << 8)), (unsigned)n);
    ble_lock();
    if (s_con_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con_handle);
    ble_unlock();
    return false;
}

// Usable §6 payload bytes per frame = negotiated MTU − ATT(3) − frame header(4),
// additionally capped so payload + hdr(4) + ATT(3) + L2CAP(4) fits one 251-byte
// HCI ACL packet: anything larger gets fragmented across HCI packets and the
// CYW43 transport wedges mid-fragmentation (hardware finding: STARVED with
// free slots remaining — the outgoing packet buffer stuck reserved).
static uint16_t frame_payload_max(void) {
    uint16_t mtu = att_server_get_mtu(s_con_handle);
    if (mtu < 23) mtu = 23;
    uint16_t p = (uint16_t)(mtu - 3 - 4);
    if (p > 240) p = 240;   // 240 + 4 + 3 + 4 = 251, single fragment always
    return p;
}

// Split a buffer into §6 frames on STATUS: [reqId][flags][seq16 LE][payload].
static uint8_t s_frame[4 + 500];
static bool send_response(uint8_t req_id, const uint8_t *payload, uint32_t len, bool is_err) {
    uint16_t pmax = frame_payload_max();
    uint16_t seq = 0;
    uint32_t off = 0;
    do {
        uint16_t chunk = (len - off > pmax) ? pmax : (uint16_t)(len - off);
        bool last = (off + chunk >= len);
        s_frame[0] = req_id;
        s_frame[1] = (uint8_t)((last ? 0x01 : 0) | (is_err ? 0x02 : 0));
        s_frame[2] = (uint8_t)(seq & 0xFF);
        s_frame[3] = (uint8_t)(seq >> 8);
        if (chunk) memcpy(&s_frame[4], payload + off, chunk);
        if (!notify_raw(s_frame, (uint16_t)(4 + chunk))) return false;
        off += chunk;
        seq++;
    } while (off < len);
    return true;
}

static bool send_error(uint8_t req_id, const char *code) {
    char b[64];
    int n = snprintf(b, sizeof(b), "{\"error\":\"%s\"}", code);
    return send_response(req_id, (const uint8_t *)b, (uint32_t)n, true);
}

// Minimal JSON string escaping for FAT filenames.
static int json_escape(char *out, int outsz, const char *in) {
    int o = 0;
    for (const char *p = in; *p && o < outsz - 7; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch < 0x20)          { o += snprintf(out + o, outsz - o, "\\u%04x", ch); }
        else                          { out[o++] = (char)ch; }
    }
    out[o] = '\0';
    return o;
}

// SHA-256 of a file → 64 lowercase hex chars (empty on error).
static void sha256_file(const char *path, char *hex) {
    hex[0] = '\0';
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    pico_sha256_state_t s;
    if (pico_sha256_start_blocking(&s, SHA256_BIG_ENDIAN, false) != PICO_OK) { f_close(&f); return; }
    static uint8_t rb[512];
    UINT br;
    while (f_read(&f, rb, sizeof(rb), &br) == FR_OK && br) pico_sha256_update(&s, rb, br);
    sha256_result_t res;
    pico_sha256_finish(&s, &res);
    f_close(&f);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", res.bytes[i]);
}

// jsmn token helpers (commands are flat objects, so keys step two at a time).
static bool tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int n = t->end - t->start;
    return t->type == JSMN_STRING && (int)strlen(s) == n && strncmp(js + t->start, s, (size_t)n) == 0;
}
static int find_val(const char *js, const jsmntok_t *t, int count, const char *key) {
    for (int i = 1; i + 1 < count; i += 2)
        if (tok_eq(js, &t[i], key)) return i + 1;
    return -1;
}
static void tok_str(const char *js, const jsmntok_t *t, char *out, int outsz) {
    int n = t->end - t->start;
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, js + t->start, (size_t)n);
    out[n] = '\0';
}
static long tok_int(const char *js, const jsmntok_t *t) {
    char b[16];
    tok_str(js, t, b, sizeof(b));
    return strtol(b, NULL, 10);
}

// op:list → JSON array [{name,isDir,size}] for one directory.
static bool cmd_list(uint8_t req_id, const char *path) {
    static char buf[8192];
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, path[0] ? path : "/") != FR_OK) return send_error(req_id, "ENOENT");
    int n = 0;
    buf[n++] = '[';
    bool first = true;
    FRESULT rr;
    while ((rr = f_readdir(&dir, &fi)) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;
        if (n > (int)sizeof(buf) - 160) break;   // cap: very large dirs are truncated
        if (!first) buf[n++] = ',';
        first = false;
        n += snprintf(buf + n, sizeof(buf) - n, "{\"name\":\"");
        n += json_escape(buf + n, (int)sizeof(buf) - n, fi.fname);
        n += snprintf(buf + n, sizeof(buf) - n, "\",\"isDir\":%s,\"size\":%lu}",
                      (fi.fattrib & AM_DIR) ? "true" : "false", (unsigned long)fi.fsize);
    }
    f_closedir(&dir);
    // A failed read must be an ERROR, not a silently-empty listing — the app
    // can't tell "[]" from "SD wedged" otherwise (seen after mode re-entry).
    if (rr != FR_OK) {
        printf("[cmd] list readdir failed rc=%d path=%s\n", (int)rr, path);
        return send_error(req_id, "EIO");
    }
    buf[n++] = ']';
    return send_response(req_id, (const uint8_t *)buf, (uint32_t)n, false);
}

// op:stat → {size,sha256} for one file.
static bool cmd_stat(uint8_t req_id, const char *path) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return send_error(req_id, "ENOENT");
    char hex[65];
    hex[0] = '\0';
    if (!(fi.fattrib & AM_DIR)) sha256_file(path, hex);
    char out[128];
    int n = snprintf(out, sizeof(out), "{\"size\":%lu,\"sha256\":\"%s\"}",
                     (unsigned long)fi.fsize, hex);
    return send_response(req_id, (const uint8_t *)out, (uint32_t)n, false);
}

// Set by every successful SD mutation (wcommit/delete/rename/setlabel) so the
// browser can rebuild its stale listing after Connect mode ends.
static volatile bool s_sd_changed;

bool ota_sd_changed(void) {
    bool c = s_sd_changed;
    s_sd_changed = false;
    return c;
}

// §5b thumbnail sidecar: "<X.MDV>.thumb" (full filename + .thumb) rides along
// with its cartridge on delete/rename — best-effort, never fails the main op.
static bool is_mdv(const char *path) {
    size_t n = strlen(path);
    return n > 4 && strcasecmp(path + n - 4, ".mdv") == 0;
}

// §5 cartridge label (experimental; app gates it behind a settings toggle).
// QLay-dialect MDV images only: 255 sectors × 686 bytes = 174,930. Per sector
// record: 12-byte preamble, then the 16-byte sector header — flag $FF, sector
// number, 10-byte medium name, 2-byte random word, 16-bit checksum = sum of
// the 14 header bytes with preset $0F0F, stored little-endian. Format facts
// verified against Martin Head's "MDV image file formats" and the
// MicroPicoDrive firmware's fix_cartridge_checksums().
#define MDV_IMAGE_SIZE   174930u
#define MDV_SECTOR_SIZE  686u
#define MDV_SECTORS      255u
#define MDV_HDR_OFF      12u    // sector header start within a record
#define MDV_NAME_OFF     14u    // 10-byte medium name
#define MDV_NAME_LEN     10u
#define MDV_CSUM_OFF     26u    // header checksum (LE), covers bytes 12..25

// Blank/damaged sectors (header flag != $FF) are treated as a NORMAL part of a
// cartridge (gusmanb MicroDriveTools filters on the flag, never errors):
// setlabel copies them through verbatim, getlabel scans for the first valid
// header. Rollback guard: set to 0 to restore the strict v1.156 rule — EINVAL
// on ANY invalid header, original untouched — if leniency ever proves to
// mislabel an image in the field.
#define MDV_LABEL_SKIP_BLANK 1

static void build_info_json(void);  // defined below (INFO json)

// op:getlabel → {"label":"…"} — medium name from the first sector with a
// valid header (blank/damaged sectors have flag != $FF and are normal),
// trailing spaces trimmed. EINVAL = wrong size or no valid header at all.
static bool cmd_getlabel(uint8_t req_id, const char *path) {
    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return send_error(req_id, "ENOENT");
    if (fi.fattrib & AM_DIR) return send_error(req_id, "EISDIR");
    if (fi.fsize != MDV_IMAGE_SIZE) return send_error(req_id, "EINVAL");
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return send_error(req_id, "EIO");
    uint8_t hdr[16];
    bool found = false;
    FRESULT fr = FR_OK;
    // Guard off → only record 0 is consulted (strict v1.156 behavior).
    const uint32_t scan = MDV_LABEL_SKIP_BLANK ? MDV_SECTORS : 1u;
    for (uint32_t s = 0; s < scan && !found; s++) {
        UINT br = 0;
        fr = f_lseek(&f, s * MDV_SECTOR_SIZE + MDV_HDR_OFF);
        if (fr == FR_OK) fr = f_read(&f, hdr, sizeof(hdr), &br);
        if (fr != FR_OK || br != sizeof(hdr)) { fr = FR_INT_ERR; break; }
        found = hdr[0] == 0xFF;
    }
    f_close(&f);
    if (fr != FR_OK) return send_error(req_id, "EIO");
    if (!found) return send_error(req_id, "EINVAL");
    char label[MDV_NAME_LEN + 1];
    memcpy(label, &hdr[2], MDV_NAME_LEN);
    int n = MDV_NAME_LEN;
    while (n > 0 && label[n - 1] == ' ') n--;
    label[n] = '\0';
    char out[64];
    int o = snprintf(out, sizeof(out), "{\"label\":\"");
    o += json_escape(out + o, (int)sizeof(out) - o, label);
    o += snprintf(out + o, sizeof(out) - o, "\"}");
    return send_response(req_id, (const uint8_t *)out, (uint32_t)o, false);
}

// op:setlabel → rewrite the medium name in every sector header + recompute
// each header checksum. Never patches in place: patched copy to <path>.part,
// then atomic rename over the original (a partial rename would make QDOS
// reject the cartridge as "bad or changed medium").
static bool cmd_setlabel(uint8_t req_id, const char *path, const char *label) {
    size_t ln = strlen(label);
    bool valid = ln >= 1 && ln <= MDV_NAME_LEN;
    for (size_t i = 0; valid && i < ln; i++)
        if (label[i] < 0x20 || label[i] > 0x7E) valid = false;
    if (!valid) return send_error(req_id, "EINVAL");
    uint8_t name[MDV_NAME_LEN];
    memset(name, ' ', sizeof(name));   // FORMAT-style space padding
    memcpy(name, label, ln);

    FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return send_error(req_id, "ENOENT");
    if (fi.fattrib & AM_DIR) return send_error(req_id, "EISDIR");
    if (fi.fsize != MDV_IMAGE_SIZE) return send_error(req_id, "EINVAL");

    DWORD fclst;
    FATFS *fs;
    if (f_getfree("", &fclst, &fs) != FR_OK ||
        (uint64_t)fclst * fs->csize * 512u < MDV_IMAGE_SIZE)
        return send_error(req_id, "ENOSPC");

    char tmp[110];
    if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp))
        return send_error(req_id, "EPROTO");

    FIL src, dst;
    if (f_open(&src, path, FA_READ) != FR_OK) return send_error(req_id, "EIO");
    if (f_open(&dst, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        f_close(&src);
        return send_error(req_id, "EIO");
    }
    static uint8_t sec[MDV_SECTOR_SIZE];
    const char *err = NULL;
    for (uint32_t s = 0; s < MDV_SECTORS && !err; s++) {
        UINT n = 0;
        if (f_read(&src, sec, sizeof(sec), &n) != FR_OK || n != sizeof(sec)) { err = "EIO"; continue; }
        if (sec[MDV_HDR_OFF] != 0xFF) {
#if MDV_LABEL_SKIP_BLANK
            // Blank/damaged sector (no valid header) — copy it through
            // VERBATIM (a bare continue would drop 686 bytes and corrupt the
            // image); only valid headers get relabeled.
            if (f_write(&dst, sec, sizeof(sec), &n) != FR_OK || n != sizeof(sec)) err = "EIO";
            continue;
#else
            err = "EINVAL";   // strict v1.156 rule: reject the whole image
            continue;
#endif
        }
        memcpy(&sec[MDV_NAME_OFF], name, MDV_NAME_LEN);
        uint16_t sum = 0x0f0f;
        for (uint32_t i = MDV_HDR_OFF; i < MDV_CSUM_OFF; i++) sum = (uint16_t)(sum + sec[i]);
        sec[MDV_CSUM_OFF]     = (uint8_t)(sum & 0xFF);
        sec[MDV_CSUM_OFF + 1] = (uint8_t)(sum >> 8);
        if (f_write(&dst, sec, sizeof(sec), &n) != FR_OK || n != sizeof(sec)) err = "EIO";
    }
    f_close(&src);
    f_close(&dst);
    if (err) { f_unlink(tmp); return send_error(req_id, err); }
    f_unlink(path);                               // FatFs rename won't overwrite
    if (f_rename(tmp, path) != FR_OK) { f_unlink(tmp); return send_error(req_id, "EIO"); }
    s_sd_changed = true;
    build_info_json();
    printf("[cmd] setlabel %s -> '%s'\n", path, label);
    return send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
}

// op:read → raw file bytes [offset, offset+len) streamed as §6 frames.
static bool cmd_read(uint8_t req_id, const char *path, uint32_t offset, uint32_t len) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return send_error(req_id, "ENOENT");
    FSIZE_t total = f_size(&f);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(s_xfer_name, base, sizeof(s_xfer_name) - 1);
    s_xfer_name[sizeof(s_xfer_name) - 1] = '\0';
    s_xfer_send = true;   // device is the sender on a pull
    if (offset && f_lseek(&f, offset) != FR_OK) { f_close(&f); return send_error(req_id, "ESEEK"); }
    uint16_t pmax = frame_payload_max();
    uint16_t seq = 0;
    uint32_t sent = 0;
    bool ok = true;
    while (sent < len) {
        uint16_t want = (len - sent > pmax) ? pmax : (uint16_t)(len - sent);
        UINT br = 0;
        if (f_read(&f, &s_frame[4], want, &br) != FR_OK) { ok = false; break; }
        bool eof = (br < want);
        bool last = (sent + br >= len) || eof;
        s_frame[0] = req_id;
        s_frame[1] = (uint8_t)(last ? 0x01 : 0);
        s_frame[2] = (uint8_t)(seq & 0xFF);
        s_frame[3] = (uint8_t)(seq >> 8);
        if (!notify_raw(s_frame, (uint16_t)(4 + br))) { ok = false; break; }
        sent += br;
        seq++;
        if (eof) break;
    }
    f_close(&f);
    // Device is the sender; publish progress for the Connect screen.
    s_xfer_pct = total ? (int)(((uint64_t)(offset + sent) * 100) / total) : 100;
    s_xfer_active = true;
    s_xfer_ms = to_ms_since_boot(get_absolute_time());
    return ok;
}

// ── §5b push machinery (UI loop side) ────────────────────────────────────────
static void ring_read(uint8_t *dst, uint32_t n) {
    uint32_t pos = s_ring_tail % OTA_RING_SIZE;
    uint32_t first = OTA_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(dst, &s_ring[pos], first);
    if (n > first) memcpy(dst + first, &s_ring[0], n - first);
    s_ring_tail += n;
}

static void push_cleanup(bool unlink_tmp) {
    if (!s_w_active) return;
    s_w_active = false;      // gate the producer off first
    s_w_disc = false;
    f_close(&s_w_fil);
    if (unlink_tmp) f_unlink(s_w_tmp);
    s_xfer_active = false;
}

// wack: DATA frame with the wbegin reqId, LAST *not* set, {"nextOffset":N}.
static bool send_wack(void) {
    char js[32];
    int n = snprintf(js, sizeof(js), "{\"nextOffset\":%lu}", (unsigned long)s_w_received);
    s_frame[0] = s_w_reqid;
    s_frame[1] = 0;
    s_frame[2] = 0;
    s_frame[3] = 0;
    memcpy(&s_frame[4], js, (size_t)n);
    bool ok = notify_raw(s_frame, (uint16_t)(4 + n));
    if (ok) s_w_acked = s_w_received;
    return ok;
}

// Stale ".part" leftovers (disconnect mid-push) are deleted at mode entry.
static void cleanup_stale_parts(void) {
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, "/") != FR_OK) return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t l = strlen(fi.fname);
        if (l > 5 && strcasecmp(fi.fname + l - 5, ".part") == 0) {
            char p[112];
            snprintf(p, sizeof(p), "/%s", fi.fname);
            f_unlink(p);
            printf("[push] removed stale %s\n", p);
        }
    }
    f_closedir(&d);
}

// Drain ring records into the .part file; out-of-sequence frames are discarded
// until the resume wack resyncs the app (s_w_expected = -1 adopts the next seq).
static void push_drain(void) {
    static uint8_t payload[512];
    while (s_w_active && s_ring_head - s_ring_tail >= 5) {
        uint8_t hdr[5];
        ring_read(hdr, 5);
        uint16_t plen = (uint16_t)(hdr[3] | (hdr[4] << 8));
        if (plen > sizeof(payload)) { push_cleanup(true); return; }   // corrupt record
        ring_read(payload, plen);
        uint16_t seq = (uint16_t)(hdr[1] | (hdr[2] << 8));
        s_w_last_rx_ms = to_ms_since_boot(get_absolute_time());
        if (s_w_expected < 0) s_w_expected = seq;
        if (!s_w_gap && seq == (uint16_t)s_w_expected) {
            UINT bw = 0;
            if (f_write(&s_w_fil, payload, plen, &bw) != FR_OK || bw != plen) {
                push_cleanup(true);
                send_error(s_w_reqid, "EIO");
                return;
            }
            s_w_received += plen;
            s_w_expected = (uint16_t)(seq + 1);
            if (hdr[0] & 0x01) s_w_last_seen = true;
            if (s_w_size) s_xfer_pct = (int)((uint64_t)s_w_received * 100 / s_w_size);
            s_xfer_ms = s_w_last_rx_ms;
        } else {
            s_w_gap = true;   // dropped frame(s): ignore until the app rewinds
        }
    }
}

// Acks + timeouts, called each loop pass: window boundary or LAST → immediate
// wack; gap or unacked tail + inbound idle → resume wack; long idle → abort.
static void push_service(void) {
    if (!s_w_active) return;
    if (s_w_disc) { push_cleanup(true); return; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_w_received - s_w_acked >= W_ACK_WINDOW ||
        (s_w_last_seen && s_w_received > s_w_acked)) {
        send_wack();
    } else if ((s_w_gap || s_w_received > s_w_acked) &&
               now - s_w_last_rx_ms > W_GAP_IDLE_MS) {
        send_wack();
        s_w_expected = -1;   // resync: adopt the next frame's seq
        s_w_gap = false;
    }
    if (now - s_w_last_rx_ms > W_SESSION_TIMEOUT_MS) {
        printf("[push] session timed out\n");
        push_cleanup(true);
    }
}

// Refresh the prebuilt INFO JSON (name/unitId/fw + live SD status).
static void build_info_json(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    char uid[17];
    for (int i = 0; i < 8; i++) snprintf(uid + i * 2, 3, "%02x", id.id[i]);
    bool inserted = sd_menu_available();
    uint64_t freeB = 0, totB = 0;
    if (inserted) {
        DWORD fclst;
        FATFS *fs;
        if (f_getfree("", &fclst, &fs) == FR_OK) {
            uint64_t cs = (uint64_t)fs->csize * 512u;
            totB  = (uint64_t)(fs->n_fatent - 2) * cs;
            freeB = (uint64_t)fclst * cs;
        }
    }
    s_info_len = snprintf(s_info_json, sizeof(s_info_json),
        "{\"name\":\"%s\",\"unitId\":\"%s\",\"fw\":\"%s\","
        "\"sd\":{\"inserted\":%s,\"freeBytes\":%llu,\"totalBytes\":%llu}}",
        s_devname, uid, ota_fw_version_string(), inserted ? "true" : "false",
        (unsigned long long)freeB, (unsigned long long)totB);
}

// §5 "health": on-die temperature in tenths of °C (same sensor read and
// RP2040/RP2350 formula as sys_info.c).
static int temp_tenths(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += adc_read();
    float v = (acc / 8.0f) * 3.3f / 4096.0f;
    float t = 27.0f - (v - 0.706f) / 0.001721f;
    return (int)(t * 10.0f + (t >= 0 ? 0.5f : -0.5f));
}

// Parse and serve one JSON command from s_cmd_buf.
static void process_command(void) {
    char js[256];
    uint16_t jl;
    ble_lock();
    jl = s_cmd_len;
    if (jl > sizeof(js) - 1) jl = sizeof(js) - 1;
    memcpy(js, s_cmd_buf, jl);
    js[jl] = '\0';
    s_cmd_ready = false;
    ble_unlock();

    jsmn_parser p;
    jsmntok_t tok[32];
    jsmn_init(&p);
    int c = jsmn_parse(&p, js, jl, tok, 32);
    if (c < 1 || tok[0].type != JSMN_OBJECT) return;   // no reqId to answer to

    int vi = find_val(js, tok, c, "reqId");
    uint8_t req_id = vi >= 0 ? (uint8_t)tok_int(js, &tok[vi]) : 0;
    int oi = find_val(js, tok, c, "op");
    if (oi < 0) { send_error(req_id, "EPROTO"); return; }
    char op[16];
    tok_str(js, &tok[oi], op, sizeof(op));

    // MicroPicoDrive §5b guard: mutations + reboot are EBUSY while a
    // cartridge is mounted or the QL is using the drive (see file header).
    if (cartridge_busy() &&
        (!strcmp(op, "wbegin") || !strcmp(op, "delete") ||
         !strcmp(op, "rename") || !strcmp(op, "setlabel") ||
         !strcmp(op, "reboot"))) {
        send_error(req_id, "EBUSY");
        return;
    }

    if (!strcmp(op, "hello")) {
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "ping")) {
        // §5 heartbeat. First ping arms the liveness watchdog for this session
        // — clients that never ping (older app versions) are never dropped
        // for idling. The reply is the ack the app watches for.
        s_hb_armed = true;
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "reboot")) {
        // §5 Pico Tools. Ack first so the app can tell "accepted" from "unit
        // vanished"; the reset fires 500 ms later, well after the notify flushed.
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
        uiext_ota_status("Restarting", "");
        printf("[cmd] reboot in 500 ms\n");
        watchdog_hw->scratch[0] = OTA_REBOOT_TO_CONNECT_MAGIC;   // boot into Connect mode
        watchdog_reboot(0, 0, 500);
    } else if (!strcmp(op, "identify")) {
        // §5 Pico Tools. The connect loop flashes the screen until the deadline.
        s_identify_until = to_ms_since_boot(get_absolute_time()) + 3000;
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "setname")) {
        // §5 Pico Tools: 1–16 printable ASCII, persisted via the bond-store TLV.
        int ni = find_val(js, tok, c, "name");
        if (ni < 0) { send_error(req_id, "EPROTO"); return; }
        char name[DEV_NAME_MAX + 8];
        tok_str(js, &tok[ni], name, sizeof(name));
        size_t ln = strlen(name);
        bool valid = ln >= 1 && ln <= DEV_NAME_MAX;
        for (size_t i = 0; valid && i < ln; i++)
            if (name[i] < 0x20 || name[i] > 0x7E) valid = false;
        if (!valid) { send_error(req_id, "EINVAL"); return; }
        ble_lock();
        const btstack_tlv_t *tlv = NULL;
        void *ctx = NULL;
        btstack_tlv_get_instance(&tlv, &ctx);
        if (tlv) tlv->store_tag(ctx, DEV_NAME_TAG, (const uint8_t *)name, (uint32_t)ln);
        ble_unlock();
        memcpy(s_devname, name, ln + 1);
        build_info_json();   // INFO reflects the new name immediately; the scan
                             // response picks it up on the next advertising start
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "health")) {
        // §5 Pico Tools: cheap status snapshot, app polls ~5 s while visible.
        extern char __StackLimit, __bss_end__;
        uint32_t heap_total = (uint32_t)(&__StackLimit - &__bss_end__);
        uint32_t heap_used = (uint32_t)mallinfo().uordblks;
        uint32_t heap_free = heap_total > heap_used ? heap_total - heap_used : 0;
        int tenths = temp_tenths();
        char b[128];
        int n = snprintf(b, sizeof(b),
            "{\"uptimeS\":%lu,\"tempC\":%d.%d,\"freeHeapB\":%lu,\"fw\":\"%s\",\"slot\":\"%c\"}",
            (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000u),
            tenths / 10, tenths < 0 ? -(tenths % 10) : tenths % 10,
            (unsigned long)heap_free, ota_fw_version_string(),
            s_active_slot == 1 ? 'B' : 'A');
        send_response(req_id, (const uint8_t *)b, (uint32_t)n, false);
    } else if (!strcmp(op, "info")) {
        build_info_json();
        send_response(req_id, (const uint8_t *)s_info_json, (uint32_t)s_info_len, false);
    } else if (!strcmp(op, "list")) {
        char path[128] = "/";
        int pi = find_val(js, tok, c, "path");
        if (pi >= 0) tok_str(js, &tok[pi], path, sizeof(path));
        cmd_list(req_id, path);
    } else if (!strcmp(op, "stat")) {
        int pi = find_val(js, tok, c, "path");
        if (pi < 0) { send_error(req_id, "EPROTO"); return; }
        char path[128];
        tok_str(js, &tok[pi], path, sizeof(path));
        cmd_stat(req_id, path);
    } else if (!strcmp(op, "read")) {
        int pi = find_val(js, tok, c, "path");
        int li = find_val(js, tok, c, "len");
        if (pi < 0 || li < 0) { send_error(req_id, "EPROTO"); return; }
        char path[128];
        tok_str(js, &tok[pi], path, sizeof(path));
        int oo = find_val(js, tok, c, "offset");
        uint32_t off = oo >= 0 ? (uint32_t)tok_int(js, &tok[oo]) : 0;
        uint32_t len = (uint32_t)tok_int(js, &tok[li]);
        cmd_read(req_id, path, off, len);
    } else if (!strcmp(op, "wbegin")) {
        int pi = find_val(js, tok, c, "path");
        int si = find_val(js, tok, c, "size");
        int hi = find_val(js, tok, c, "sha256");
        if (pi < 0 || si < 0 || hi < 0) { send_error(req_id, "EPROTO"); return; }
        if (s_w_active) push_cleanup(true);           // stale session: replace
        char path[96];
        tok_str(js, &tok[pi], path, sizeof(path));
        uint32_t size = (uint32_t)tok_int(js, &tok[si]);
        // Free-space guard (with one cluster's slack for FAT bookkeeping).
        DWORD fclst;
        FATFS *fs;
        if (f_getfree("", &fclst, &fs) == FR_OK &&
            (uint64_t)size + 65536 > (uint64_t)fclst * fs->csize * 512u) {
            send_error(req_id, "ENOSPC");
            return;
        }
        if (snprintf(s_w_tmp, sizeof(s_w_tmp), "%s.part", path) >= (int)sizeof(s_w_tmp)) {
            send_error(req_id, "EPROTO");
            return;
        }
        strncpy(s_w_path, path, sizeof(s_w_path) - 1);
        s_w_path[sizeof(s_w_path) - 1] = '\0';
        tok_str(js, &tok[hi], s_w_sha, sizeof(s_w_sha));
        if (f_open(&s_w_fil, s_w_tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
            send_error(req_id, "EIO");
            return;
        }
        s_w_size = size;
        s_w_received = s_w_acked = 0;
        s_w_expected = -1;
        s_w_gap = false;
        s_w_last_seen = false;
        s_w_disc = false;
        s_w_last_rx_ms = to_ms_since_boot(get_absolute_time());
        s_ring_head = s_ring_tail = 0;
        s_w_reqid = req_id;
        s_w_active = true;                            // producer gate — set last
        // "Receiving <name>" + counter on the Connect screen.
        const char *base = strrchr(path, '/');
        strncpy(s_xfer_name, base ? base + 1 : path, sizeof(s_xfer_name) - 1);
        s_xfer_name[sizeof(s_xfer_name) - 1] = '\0';
        s_xfer_send = false;
        s_xfer_pct = 0;
        s_xfer_active = true;
        s_xfer_ms = s_w_last_rx_ms;
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "wcommit")) {
        if (!s_w_active) { send_error(req_id, "ESTATE"); return; }
        s_w_active = false;                           // producer off; finish locally
        f_close(&s_w_fil);
        bool ok = (s_w_received == s_w_size);
        if (ok) {
            char hex[65];
            sha256_file(s_w_tmp, hex);
            ok = (strcasecmp(hex, s_w_sha) == 0);
        }
        s_xfer_active = false;
        if (!ok) {
            f_unlink(s_w_tmp);
            send_error(req_id, "ECHECKSUM");
            return;
        }
        f_unlink(s_w_path);                           // FatFs rename won't overwrite
        if (f_rename(s_w_tmp, s_w_path) != FR_OK) {
            f_unlink(s_w_tmp);
            send_error(req_id, "EIO");
            return;
        }
        s_sd_changed = true;
        build_info_json();                            // free space changed
        printf("[push] %s committed (%lu bytes)\n", s_w_path, (unsigned long)s_w_size);
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "getlabel")) {
        int pi = find_val(js, tok, c, "path");
        if (pi < 0) { send_error(req_id, "EPROTO"); return; }
        char path[96];
        tok_str(js, &tok[pi], path, sizeof(path));
        cmd_getlabel(req_id, path);
    } else if (!strcmp(op, "setlabel")) {
        int pi = find_val(js, tok, c, "path");
        int li = find_val(js, tok, c, "label");
        if (pi < 0 || li < 0) { send_error(req_id, "EPROTO"); return; }
        char path[96], label[24];
        tok_str(js, &tok[pi], path, sizeof(path));
        tok_str(js, &tok[li], label, sizeof(label));
        cmd_setlabel(req_id, path, label);
    } else if (!strcmp(op, "delete")) {
        int pi = find_val(js, tok, c, "path");
        if (pi < 0) { send_error(req_id, "EPROTO"); return; }
        char path[96];
        tok_str(js, &tok[pi], path, sizeof(path));
        FILINFO st;
        if (f_stat(path, &st) != FR_OK) { send_error(req_id, "ENOENT"); return; }
        if (st.fattrib & AM_DIR) { send_error(req_id, "EISDIR"); return; }
        if (f_unlink(path) != FR_OK) { send_error(req_id, "EIO"); return; }
        if (is_mdv(path)) {   // §5b: sidecar rides along, best-effort
            char th[110];
            if (snprintf(th, sizeof(th), "%s.thumb", path) < (int)sizeof(th))
                f_unlink(th);
        }
        s_sd_changed = true;
        build_info_json();
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else if (!strcmp(op, "rename")) {
        int fi = find_val(js, tok, c, "from");
        int ti = find_val(js, tok, c, "to");
        if (fi < 0 || ti < 0) { send_error(req_id, "EPROTO"); return; }
        char from[96], to[96];
        tok_str(js, &tok[fi], from, sizeof(from));
        tok_str(js, &tok[ti], to, sizeof(to));
        FILINFO st;
        if (f_stat(to, &st) == FR_OK) { send_error(req_id, "EEXIST"); return; }
        FRESULT fr = f_rename(from, to);
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) { send_error(req_id, "ENOENT"); return; }
        if (fr != FR_OK) { send_error(req_id, "EIO"); return; }
        if (is_mdv(from)) {   // §5b: sidecar follows the cartridge, best-effort
            char ft[110], tt[110];
            FILINFO ts;
            if (snprintf(ft, sizeof(ft), "%s.thumb", from) < (int)sizeof(ft) &&
                snprintf(tt, sizeof(tt), "%s.thumb", to) < (int)sizeof(tt) &&
                f_stat(ft, &ts) == FR_OK) {
                f_unlink(tt);          // FatFs rename won't overwrite a stale one
                f_rename(ft, tt);
            }
        }
        s_sd_changed = true;
        build_info_json();
        send_response(req_id, (const uint8_t *)"{\"ok\":true}", 11, false);
    } else {
        send_error(req_id, "ENOSYS");
    }
}

// Config-menu entry: advertise + serve JSON commands until cancelled/idle.
void ota_run_connect_mode(void) {
    if (!ble_up()) { uiext_ota_status("Connect", "Radio init failed"); sleep_ms(2000); return; }
    s_pairing_allowed = true;    // allow first-time passkey pairing from the app
    s_passkey_pending = false;
    s_connect_mode = true;
    s_cmd_ready = false;
    s_xfer_active = false;
    s_name_read_started = false;
    s_name_pending = false;
    s_state = OTA_ADVERTISING;
    build_info_json();
    if (sd_menu_available()) cleanup_stale_parts();   // §5b: stale .part sweep
    ble_advertise(true);

    // Swallow a still-held SELECT/CONFIG from the menu entry / K4 shortcut.
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0) sleep_ms(20);
    sleep_ms(120);

    enum { SCR_NONE, SCR_WAIT, SCR_PASSKEY, SCR_CONN, SCR_XFER } shown = SCR_NONE;
    int dots = 0, shown_pct = -1;
    uint32_t last_dot = 0, passkey_until = 0;
    bool was_connected = false;
    bool identify_on = false;                // §5 identify: white phase currently shown
    absolute_time_t deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);

    while (true) {
        if (btn_cancel_pressed()) break;
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Client dropped → re-advertise so it (or another) can reconnect.
        bool connected = (s_state == OTA_CONNECTED);
        // Belt-and-braces: verify the handle still exists in the stack, so a
        // missed disconnect event can never leave the screen stuck on
        // "Connected" (observed once in the field).
        if (connected) {
            ble_lock();
            bool alive = (s_con_handle != HCI_CON_HANDLE_INVALID) &&
                         (gap_get_connection_type(s_con_handle) != GAP_CONNECTION_INVALID);
            ble_unlock();
            if (!alive) {
                s_con_handle = HCI_CON_HANDLE_INVALID;
                s_status_notify_on = false;
                s_encrypted = false;
                if (s_w_active) s_w_disc = true;
                s_state = OTA_ADVERTISING;
                connected = false;
            }
        }
        if (was_connected && !connected) ble_advertise(true);
        was_connected = connected;

        // §5 heartbeat: once the app has pinged, prolonged inbound silence
        // means the peer is dead (phone asleep / app killed) — drop the link
        // so the unit returns to "Waiting Connection" instead of a zombie.
        if (connected && s_hb_armed &&
            now - s_last_inbound_ms > UIEXT_HB_TIMEOUT_MS) {
            printf("[hb] no inbound for %u ms — dropping link\n",
                   (unsigned)(now - s_last_inbound_ms));
            s_hb_armed = false;
            ble_lock();
            if (s_con_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con_handle);
            ble_unlock();
        }

        // A new passkey to display (first-time pairing) wins over everything
        // until the link is encrypted or the display times out.
        if (s_passkey_pending) {
            char pk[16];
            snprintf(pk, sizeof(pk), "%06lu", (unsigned long)s_passkey);
            s_passkey_pending = false;
            uiext_ota_status("Pairing", "");
            uiext_ota_big_text(pk, UIEXT_BIG_CENTER_Y);   // PIN, 1.5x, body-centered
            shown = SCR_PASSKEY;
            passkey_until = now + 30000;
        }
        bool holding_passkey = shown == SCR_PASSKEY && !s_encrypted && now < passkey_until;

        // Expire the transfer view a while after the last read window (long
        // enough that a slow inter-window gap doesn't flip back to "Connected").
        if (s_xfer_active && now - s_xfer_ms > 3000) s_xfer_active = false;

        int desired = holding_passkey ? SCR_PASSKEY
                    : connected       ? (s_xfer_active ? SCR_XFER : SCR_CONN)
                    :                    SCR_WAIT;

        if (desired == SCR_WAIT) {
            if (shown != SCR_WAIT) {                  // static title + name, anim below
                shown = SCR_WAIT; dots = 0; last_dot = now;
                uiext_ota_wait("Waiting Connection", s_devname);
                uiext_ota_wait_anim(0);   // titled layout: classic anim spot
            } else if (now - last_dot > UIEXT_WAIT_ANIM_MS) {  // animate only the wait line
                last_dot = now; dots = (dots + 1) % UIEXT_WAIT_ANIM_FRAMES;
                uiext_ota_wait_anim(dots);
            }
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
        } else if (desired == SCR_CONN) {
            if (shown != SCR_CONN) { shown = SCR_CONN; uiext_ota_status("Connected", s_devname); }
            deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
        } else if (desired == SCR_XFER) {
            if (shown != SCR_XFER) {                 // draw chrome once
                shown = SCR_XFER;
                // Pushed names are phone-controlled and unclipped could reach
                // ~434 px: browser-style "NAME..EXT" pixel clip, display only.
                char nline[UIEXT_MENU_ITEM_MAX_CHARS + 3];
                if (strchr(s_xfer_name, '.'))
                    format_menu_item(s_xfer_name, nline);
                else
                    uiext_pixel_clip(nline, s_xfer_name, UIEXT_DISPLAY_WIDTH);
                uiext_ota_progress(s_xfer_send ? "Sending" : "Receiving",
                                   nline, s_xfer_pct);
                shown_pct = s_xfer_pct;
            } else if (shown_pct != s_xfer_pct) {    // then only fill + %
                shown_pct = s_xfer_pct;
                uiext_ota_progress_update(s_xfer_pct);
            }
            deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
        }
        // desired == SCR_PASSKEY: already drawn, hold it.

        // §5 identify: alternate a full-screen flash with the normal screen
        // (250 ms cadence) until the deadline — no blocking, restore = force
        // the regular draw path to repaint on the next pass. Flash color is
        // the theme inverse: white on Dark, black on Light (a white flash is
        // invisible over the Light theme's white background).
        if (s_identify_until) {
            bool over = now >= s_identify_until;
            bool on = !over && ((now / 250) & 1u) == 0u;
            if (on != identify_on) {
                identify_on = on;
                if (on) fillRectangle(0, 0, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT,
                                      uiext_theme_is_dark() ? ST7735_WHITE : ST7735_BLACK);
                else    shown = SCR_NONE;
            }
            if (over) s_identify_until = 0;
        }

        push_drain();       // §5b inbound bytes → .part file
        push_service();     // §5b wacks / resume / timeouts
        if (s_cmd_ready) process_command();
        peer_name_poll();
        sleep_ms(5);
    }

    if (s_w_active) push_cleanup(true);   // mode exit aborts any open push
    s_connect_mode = false;
    ble_down();
}

// ── Pair Device mode ─────────────────────────────────────────────────────────
void ota_run_pair_mode(void) {
    if (!ble_up()) { uiext_ota_status("Pairing", "Radio init failed"); sleep_ms(2000); return; }
    s_pairing_allowed = true;
    s_pairing_done = false;
    s_passkey_pending = false;
    s_name_read_started = false;
    s_name_pending = false;
    s_state = OTA_ADVERTISING;
    ble_advertise(true);

    // Centered: title + device name in the middle (SELECT still cancels,
    // just not advertised on screen).
    uiext_ota_status("Pairing", s_devname);

    // Entered via the long-press K4 shortcut the button may still be held —
    // swallow it so it isn't read as this mode's cancel gesture.
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(120);

    absolute_time_t deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
    bool dot_on = true;    // drawn on the first tick, then blinks mostly-on
    int  dot_tick = 0;
    while (true) {
        if (btn_cancel_pressed()) break;
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
        if (s_passkey_pending) {
            s_passkey_pending = false;
            char pk[24];
            snprintf(pk, sizeof(pk), "%06lu", (unsigned long)s_passkey);
            // PIN replaces the device name: counter's 1.5x size, body-centered.
            uiext_ota_status("Pairing", "");
            uiext_ota_big_text(pk, UIEXT_BIG_CENTER_Y);
            deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
        }
        if (s_pairing_done) {
            uiext_ota_status("Pairing", s_pairing_status == ERROR_CODE_SUCCESS
                                        ? "Paired OK" : "Pairing failed");
            sleep_ms(2500);
            break;
        }
        // Blinking pairing indicator: navy dot, top-right of the title bar,
        // until pairing completes (or the mode exits). Asymmetric blink
        // (mostly on) so the dot reads as solid colour, not a faint flash.
        dot_tick++;
        if ((dot_on && dot_tick >= 12) || (!dot_on && dot_tick >= 5) || dot_tick == 1) {
            if (dot_tick > 1) { dot_on = !dot_on; dot_tick = 0; }
            fillCircle(151, 10, 6, dot_on ? UIEXT_COLOR_PAIR_DOT
                                          : UIEXT_COLOR_CFG_TITLE_BG);
        }
        peer_name_poll();
        sleep_ms(50);
    }
    s_pairing_allowed = false;
    ble_down();
}

// ── Paired devices list ──────────────────────────────────────────────────────
static int s_bond_slot[MAX_NR_LE_DEVICE_DB_ENTRIES];   // db slot per list row

// rows[] gets the cached peer name when one exists, else the MAC; macs[] always
// the MAC (serial log). Caller holds ble_lock (TLV shared with bond storage).
static int bond_collect(char rows[][PEER_NAME_MAX], char macs[][18]) {
    int n = 0;
    for (int i = 0; i < le_device_db_max_count() && n < MAX_NR_LE_DEVICE_DB_ENTRIES; i++) {
        int addr_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        le_device_db_info(i, &addr_type, addr, NULL);
        if (addr_type == BD_ADDR_TYPE_UNKNOWN) continue;
        snprintf(macs[n], 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        if (!peer_name_get(i, rows[n], PEER_NAME_MAX))
            memcpy(rows[n], macs[n], 18);
        s_bond_slot[n++] = i;
    }
    return n;
}

static void wait_release_all(void) {
    while (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0 ||
           gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(120);
}

void ota_run_paired_list(void) {
    // Bond storage is read via the BT stack's TLV instance — init the stack
    // quietly if needed (BT controller stays powered off; no advertising).
    s_quiet = true;
    bool ok = ble_stack_init();
    s_quiet = false;
    if (!ok) {
        uiext_ota_status("Paired Devices", "Radio init failed");
        sleep_ms(2000);
        return;
    }

    while (true) {
        char rows[MAX_NR_LE_DEVICE_DB_ENTRIES][PEER_NAME_MAX];
        char macs[MAX_NR_LE_DEVICE_DB_ENTRIES][18];
        const char *row_ptrs[MAX_NR_LE_DEVICE_DB_ENTRIES];
        ble_lock();
        int n = bond_collect(rows, macs);
        ble_unlock();

        if (n == 0) {
            uiext_ota_status("Paired Devices", "No paired devices");
            wait_release_all();
            while (gpio_get(UIEXT_BTN_SELECT) != 0 && gpio_get(UIEXT_BTN_CONFIG) != 0)
                sleep_ms(20);
            wait_release_all();
            return;
        }
        // Peer names are phone-controlled (worst measured 322 px): pixel-clip
        // each row to the band budget; display only, bond data untouched.
        static char disp[MAX_NR_LE_DEVICE_DB_ENTRIES][UIEXT_MENU_ITEM_MAX_CHARS + 3];
        for (int i = 0; i < n; i++) {
            uiext_pixel_clip(disp[i], rows[i],
                             UIEXT_DISPLAY_WIDTH - (UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD));
            row_ptrs[i] = disp[i];
        }

        // One-line-per-device menu (System Tools style); K4 backs out.
        int pick = uiext_menu_pick("Paired Devices", row_ptrs, n);
        if (pick < 0) return;

        // Centered confirm: device name in the middle, Yes/No below (clipped
        // to the full centered width).
        char fname[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        uiext_pixel_clip(fname, rows[pick], UIEXT_DISPLAY_WIDTH);
        if (uiext_ota_confirm_centered("Forget Device?", fname, UIEXT_CONFIRM_TIMEOUT_MS)) {
            ble_lock();
            le_device_db_remove(s_bond_slot[pick]);
            peer_name_delete(s_bond_slot[pick]);
            ble_unlock();
            printf("[ota] bond %s forgotten\n", macs[pick]);
            uiext_ota_status("Paired Devices", "Device forgotten");
            sleep_ms(1200);
        }
    }
}

// ── Forget bonds ─────────────────────────────────────────────────────────────
void ota_run_forget_bonds(void) {
    if (!uiext_ota_confirm("Pairing", "Forget all", "paired devices?", OTA_CONFIRM_TIMEOUT_MS)) return;
    if (!ble_up()) return;
    ble_lock();
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int addr_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        le_device_db_info(i, &addr_type, addr, NULL);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
            le_device_db_remove(i);
            peer_name_delete(i);
        }
    }
    ble_unlock();
    ble_down();
    uiext_ota_screen("Pairing", "All devices", "forgotten", "");
    sleep_ms(1500);
}

// ── Update Firmware (OTA) mode ───────────────────────────────────────────────
static void ota_reset_transfer(void) {
    s_ring_head = s_ring_tail = 0;
    s_start_received = s_abort_received = false;
    s_total_size = 0;
}

void ota_run_update_mode(void) {
    ota_query_partitions();
    if (s_active_slot < 0) {
        uiext_ota_screen("Update Firmware", "No partition table", "flash it via USB", "SELECT: back");
        while (!btn_cancel_pressed()) sleep_ms(50);
        return;
    }
    const int target = 1 - s_active_slot;
    const uint32_t t_off = s_slot_off[target], t_size = s_slot_size[target];

    if (!ble_up()) { uiext_ota_status("Update Firmware", "Radio init failed"); sleep_ms(2000); return; }
    s_pairing_allowed = false;
    ota_reset_transfer();
    s_state = OTA_ADVERTISING;
    ble_advertise(true);

    // Waiting chrome like the Connect mode: current version on the name line,
    // dots12 animation below (SELECT still cancels via the confirm; no hint).
    char cur[24];
    snprintf(cur, sizeof(cur), "Current %s", ota_fw_version_string());
    uiext_ota_wait("Waiting Firmware", cur);
    uiext_ota_wait_anim(0);
    uint32_t anim_last = 0;
    int anim_frame = 0;

    static uint8_t sector_buf[FLASH_SECTOR_SIZE];
    static uint8_t sector0_buf[FLASH_SECTOR_SIZE];   // header held back until confirm
    uint32_t sector_fill = 0, flashed = 0, hashed = 0;
    bool sector0_captured = false;
    uint32_t sector0_len = 0;
    pico_sha256_state_t sha;
    bool sha_active = false;
    ota_state_t last_shown = OTA_IDLE;
    int last_pct = -1;
    absolute_time_t deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
    uint8_t fail_code = 0;
    bool done_ok = false;

    while (true) {
        if (btn_cancel_pressed()) {
            // Deliberately shorter than UIEXT_CONFIRM_TIMEOUT_MS: this dialog
            // pops mid-transfer and the BLE link has its own timeout budget.
            // Centered layout, single word (owner 2026-07-31).
            if (uiext_ota_confirm_centered("Update Firmware", "Cancel", 15000)) { fail_code = OTA_ERR_CANCEL; break; }
            if (s_state != OTA_RECEIVING) {
                // Declined: restore the waiting chrome the dialog overdrew.
                uiext_ota_wait("Waiting Firmware", cur);
                uiext_ota_wait_anim(anim_frame);
            }
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) { fail_code = OTA_ERR_TIMEOUT; break; }
        if (s_abort_received) { fail_code = OTA_ERR_CANCEL; break; }

        if (s_state == OTA_CONNECTED && last_shown != OTA_CONNECTED) {
            // No screen change: "Waiting Firmware" stays up until Receiving
            // starts (the Connected interstitial was dropped as noise).
            last_shown = OTA_CONNECTED;
            deadline = make_timeout_time_ms(OTA_IDLE_TIMEOUT_MS);
        }
        if (s_state == OTA_ADVERTISING && last_shown == OTA_CONNECTED) {
            last_shown = OTA_ADVERTISING;   // client dropped before START
            uiext_ota_wait("Waiting Firmware", cur);
            uiext_ota_wait_anim(anim_frame);
        }

        // dots12 tick while the waiting screen is up (Receiving replaces it
        // with the progress screen).
        if (s_state != OTA_RECEIVING) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - anim_last >= UIEXT_WAIT_ANIM_MS) {
                anim_last = now;
                anim_frame = (anim_frame + 1) % UIEXT_WAIT_ANIM_FRAMES;
                uiext_ota_wait_anim(anim_frame);
            }
        }

        if (s_start_received) {
            s_start_received = false;
            // Wire minor is the combined minor*100+patch value (see ota_ble.h).
            uint32_t ver_new = ((uint32_t)s_new_major << 16) | s_new_minor;
            uint32_t ver_cur = ((uint32_t)UIEXT_FW_VERSION_MAJOR << 16) | UIEXT_FW_VERNUM_MINOR;
            if (s_total_size < FLASH_SECTOR_SIZE || s_total_size > t_size) {
                notify_error(OTA_ERR_SIZE);
            } else if (ver_new <= ver_cur) {
                notify_error(OTA_ERR_VERSION);
            } else if (pico_sha256_start_blocking(&sha, SHA256_BIG_ENDIAN, false) != PICO_OK) {
                notify_error(OTA_ERR_STATE);
            } else {
                sha_active = true;
                sector_fill = flashed = hashed = 0;
                sector0_captured = false;
                s_ring_head = s_ring_tail = 0;
                s_state = OTA_RECEIVING;
                uint8_t rdy[5] = { OTA_ST_READY };
                uint32_t win = OTA_RING_SIZE;
                memcpy(&rdy[1], &win, 4);
                status_notify(rdy, sizeof(rdy));
                char fwname[24];
                fmt_combined_version(fwname, sizeof(fwname), "Firmware ",
                                     s_new_major, s_new_minor);
                uiext_ota_progress("Receiving", fwname, 0);   // same centered layout as Connect
                deadline = make_timeout_time_ms(OTA_STALL_TIMEOUT_MS);
            }
        }

        // Drain ring into sector buffer / flash
        while (s_state == OTA_RECEIVING && s_ring_tail < s_ring_head) {
            uint32_t avail = s_ring_head - s_ring_tail;
            uint32_t want = FLASH_SECTOR_SIZE - sector_fill;
            uint32_t remaining_total = s_total_size - (hashed);
            if (want > avail) want = avail;
            if (want > remaining_total - sector_fill) want = remaining_total - sector_fill;
            uint32_t pos = s_ring_tail % OTA_RING_SIZE;
            uint32_t first = OTA_RING_SIZE - pos;
            if (first > want) first = want;
            memcpy(sector_buf + sector_fill, &s_ring[pos], first);
            if (want > first) memcpy(sector_buf + sector_fill + first, &s_ring[0], want - first);
            s_ring_tail += want;
            sector_fill += want;
            deadline = make_timeout_time_ms(OTA_STALL_TIMEOUT_MS);

            bool final_partial = (hashed + sector_fill == s_total_size);
            if (sector_fill == FLASH_SECTOR_SIZE || final_partial) {
                pico_sha256_update(&sha, sector_buf, sector_fill);
                hashed += sector_fill;
                bool ok = true;
                if (!sector0_captured) {
                    memcpy(sector0_buf, sector_buf, sector_fill);
                    sector0_len = sector_fill;
                    sector0_captured = true;   // header written after confirm
                } else {
                    // sector N of the image lands at t_off + N*4K ('flashed'
                    // already counts the held-back sector 0)
                    ok = flash_write_sector(t_off + flashed, sector_buf, sector_fill);
                }
                if (!ok) { fail_code = OTA_ERR_FLASH; break; }
                flashed += sector_fill;
                sector_fill = 0;
                notify_ack();
                int pct = (int)((uint64_t)hashed * 100 / s_total_size);
                if (pct != last_pct) {
                    last_pct = pct;
                    uiext_ota_progress_update(pct);   // flicker-free counter update
                }
            }
            if (fail_code) break;
        }
        if (fail_code) break;

        // Transfer complete?
        if (s_state == OTA_RECEIVING && s_total_size && hashed == s_total_size) {
            uiext_ota_status("Update Firmware", "Verifying...");
            sha256_result_t res;
            pico_sha256_finish(&sha, &res);
            sha_active = false;
            if (memcmp(res.bytes, s_expect_sha, 32) != 0) {
                fail_code = OTA_ERR_HASH;
                break;
            }
            notify_simple(OTA_ST_VERIFIED);
            char neu[24];
            fmt_combined_version(neu, sizeof(neu), "Firmware ",
                                 s_new_major, s_new_minor);
            // Centered layout, no gap band between the version and Yes/No.
            uiext_confirm_res_t ans =
                uiext_ota_confirm_centered2("Install", neu, OTA_CONFIRM_TIMEOUT_MS);
            if (ans == UIEXT_CONFIRM_YES) {
                // Commit: write the held-back header sector — image becomes valid.
                if (!flash_write_sector(t_off, sector0_buf, sector0_len)) {
                    fail_code = OTA_ERR_FLASH;
                    break;
                }
                notify_simple(OTA_ST_COMMITTED);
                uiext_ota_status("Update Firmware", "Restarting...");
                sleep_ms(400);
                ble_down();
                watchdog_reboot(0, 0, 100);
                while (true) tight_loop_contents();
            } else {
                // X3/X4 semantics: an explicit No discards, but a TIMEOUT
                // means nobody answered — park the verified image in
                // SD:/.update so the boot-time SD update offers it again.
                bool parked = false;
                if (ans == UIEXT_CONFIRM_TIMEOUT)
                    parked = ota_park_update(t_off, sector0_buf, sector0_len);
                notify_simple(OTA_ST_DECLINED);
                flash_erase_sector(t_off);   // ensure header stays invalid
                if (parked)
                    uiext_ota_status2("Timed Out", "Saved to", "SD:/.update");
                else
                    uiext_ota_screen("Update Firmware", "Declined", "nothing changed", "");
                sleep_ms(2000);
                done_ok = true;
                break;
            }
        }
        sleep_ms(10);
    }

    if (sha_active) pico_sha256_cleanup(&sha);
    if (!done_ok && fail_code) {
        notify_error(fail_code);
        flash_erase_sector(t_off);   // never leave a half-image with a header
        // A user-initiated cancel needs no reassurance line (owner) — the
        // error exits keep "old firmware intact".
        if (fail_code == OTA_ERR_CANCEL)
            uiext_ota_status("Update Firmware", "Cancelled");
        else
            uiext_ota_screen("Update Firmware",
                             fail_code == OTA_ERR_TIMEOUT ? "Timed out" :
                             fail_code == OTA_ERR_HASH ? "Hash mismatch" : "Failed",
                             "old firmware intact", "");
        sleep_ms(2000);
    }
    ble_down();
}

// ── Revert to previous slot (no radio) ───────────────────────────────────────
void ota_run_revert(void) {
    ota_query_partitions();
    if (s_active_slot < 0 || !ota_other_slot_has_image()) {
        uiext_ota_status("Revert Firmware", "No previous firmware");
        sleep_ms(2000);
        return;
    }
    // Centered confirm: "Revert <current>" / "To <other-slot version>" (read
    // from that slot's sealed image; generic line if unreadable).
    char title[28], to[28];
    snprintf(title, sizeof(title), "Revert %s", ota_fw_version_string());
    uint16_t maj, min;
    if (ota_slot_version(1 - s_active_slot, &maj, &min))
        fmt_combined_version(to, sizeof(to), "Back to ", maj, min);
    else
        snprintf(to, sizeof(to), "Back to version ?");
    if (!uiext_ota_confirm_centered(title, to, OTA_CONFIRM_TIMEOUT_MS))
        return;
    uiext_ota_status("Revert Firmware", "Reverting...");
    // Invalidate the ACTIVE slot's header; we are executing from this slot but
    // flash_safe_execute runs the erase from RAM and we reboot immediately.
    if (!flash_erase_sector(s_slot_off[s_active_slot])) {
        uiext_ota_status("Revert Firmware", "Flash error");
        sleep_ms(2000);
        return;
    }
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}
