#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "EventMachine.h"
#include "UserInterface.h"
#include "SharedBuffers.h"
#include "SharedEvents.h"
#include "ff.h"
#include "diskio.h"     //STA_NOINIT — hot-swap card re-init in sd_fs_mount
#include "hw_config.h"  //sd_get_by_num
#include "sd_menu.h"
#include "UserInterfaceExtension.h"
#include "sd_update.h"           //both boards (trap T2) — CMake picks the flavour
#if UIEXT_SETTINGS_PERSIST
#include "pico/flash.h"          //flash_safe_execute (settings sector)
#include "hardware/flash.h"
#if UIEXT_OTA_ENABLED
#include "hardware/regs/addressmap.h"  //XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#else
#include "flash_layout.h"        //FL_SETTINGS_OFFSET (asserts a 2 MB part)
#endif
#endif
#if UIEXT_OTA_ENABLED
#include "ota_ble.h"            //reboot-to-Connect magic + Connect mode
#include "hardware/watchdog.h"  //watchdog_hw->scratch
#endif
#include "hardware/irq.h"       //IO_IRQ_BANK0 (hot-plug detect IRQ)
#include "hardware/sync.h"      //save_and_disable_interrupts (presence commit)

#define LED_ON(LED) gpio_put(LED, true)
#define LED_OFF(LED) gpio_put(LED, false)
//Debounced presence from the hot-plug supervisor below — NOT a raw pin read.
//The staggered edge connector makes the raw signal lie in both directions:
//it asserts before VCC/signals are seated and breaks after they are gone.
#define IS_UI_DISCONNECTED() (!cartPresent)
#define IN_FOLDER (fno.fattrib & AM_DIR)

#define CONCAT(DEST, SOURCE) sprintf(&DEST[strlen(DEST)],"/%s", SOURCE)
#define CONFIG_FILE_SIZE 306  // "FILE=" (5) + path padded to 300 chars + '\n' (1)
USER_INTERFACE_STATE uiState = IDLE;

FATFS fatfs;
DIR dir;
FILINFO fno;
static FIL fil; //The single open file (load/save/config are strictly sequential)
char currentPath[PATH_BUFFER_SIZE];
uint8_t currentSector = 0;

bool mdInUse = false;

CARTRIDGE_FORMAT cfInserted = NONE;

// --- Cartridge hot-plug supervisor state ---
//Finger stagger (mating order): GND and UI_DETECT are full length and mate
//FIRST; +3V3 and all signal fingers mate LAST. So insertion is debounced
//(detect low for 250 ms continuously) before any cartridge-facing pin is
//driven, and removal (detect high, even one edge) tri-states everything from
//a GPIO IRQ — by then power and signals are already gone.
#define CART_DEBOUNCE_MS   250
#define SD_ERR_BURST_LIMIT 3   //consecutive FatFs failures = surprise removal

static volatile bool     cartPresent = false;       //debounced presence
static volatile bool     cartSdInvalid = false;     //removal seen: SD state is stale
static volatile bool     cartReinitPending = false; //restart from IDLE on replug
static volatile uint32_t cartLowSinceMs = 0;        //0 = not sampling a low
static int               sdErrStreak = 0;           //burst detector counter

// --- Directory menu state ---
//Listing cap per folder: entries beyond it are dropped in FAT order (BEFORE
//the alphabetical sort, so which ones vanish is arbitrary) and the visible
//list then ends in a non-selectable "..." marker row.
#define MAX_DIR_ITEMS 64
#define MAX_NAME_LEN 64 //Raw filesystem name (LFN, truncated beyond) â€” sandbox convention

typedef struct {
    char  name[MAX_NAME_LEN];
    DWORD fsize;
    bool  is_dir;
} DirEntry;

static DirEntry  dir_entries[MAX_DIR_ITEMS];
//Display labels: '*' star-tag + "[dir]" brackets need 3 extra bytes;
//+3 slots: header, "[..]" and the "..." truncation marker
static char      disp_names[MAX_DIR_ITEMS + 3][MAX_NAME_LEN + 3];
static char     *disp_ptrs[MAX_DIR_ITEMS + 3];
static int       dir_count   = 0;
static int       item_count  = 0;  //menu items incl. header slot and [..]
static bool      has_up      = false; //[..] entry present (below root)
static bool      dir_truncated = false; //scan hit MAX_DIR_ITEMS with more left
static int       menu_offset = 0;
static DWORD     selected_fsize = 0;
static char      selected_name[MAX_NAME_LEN];
static char      configTaggedPath[PATH_BUFFER_SIZE];
static bool      cartDirty = false; //Sectors written by the QL since last load/save
static DWORD     cartCardSerial = 0; //Volume serial of the card the image was
                                     //loaded from (0 = unknown) — save guard

uint64_t delayEnd;
USER_INTERFACE_STATE uiNextState;

//Waiting-SD dots animation state
static uint32_t waitDotsNextMs = 0;
static int      waitDots = 0;

bool save_mdv_cartridge();
bool save_mpd_cartridge();
static bool mount_sd(void);
static bool read_config_file(void);
static bool try_config_autoload(void);
static bool write_config_tag(const char *full_path);
static void show_cart_ready(void);

//Shared FatFs mount (sandbox sd_fs_mount API â€” sd_check calls it too).
//Always forces a fresh mount AND a full low-level card re-init: the forced
//f_mount alone skips the SPI init sequence while the driver still thinks
//the previous card is present, so a hot-swapped card wedged ALL SD IO until
//a reboot (bench B4). Same recipe as the driver's own sd_deinit, minus the
//GPIO teardown.
FRESULT sd_fs_mount(void)
{
    sd_card_t *sd = sd_get_by_num(0);
    if (sd) {
        sd->state.m_Status |= STA_NOINIT;
        sd->state.card_type = SDCARD_NONE;
    }
    return f_mount(&fatfs, "", 1);
}

static bool mount_sd(void)
{
    bool ok = sd_fs_mount() == FR_OK;
    if(ok)
        sdErrStreak = 0; //burst detector: any success ends the streak. A
                         //failed mount does NOT count — an empty slot fails
                         //the same way and is a normal waiting state.
    return ok;
}

//Sandbox sd_menu API: card present = mount succeeds (retried on each call).
bool sd_menu_available(void)
{
    return sd_fs_mount() == FR_OK;
}

//Cheap card-presence probe for polling loops: one SPI status command (the
//library's own periodic-check hook), NOT a mount — the browser would stutter
//if it re-ran the full card init.
bool sd_menu_card_present(void)
{
    //Cartridge gone ⇒ card gone. Answer from the debounced detect state
    //instead of probing the (tri-stated) SPI lines: this is what lets the
    //blocking menu loop fall out via its SD_GONE path after a hot removal.
    if(IS_UI_DISCONNECTED())
        return false;
    sd_card_t *sd = sd_get_by_num(0);
    return sd && sd->sd_test_com && sd->sd_test_com(sd);
}

//BLE server EBUSY guard (ota_ble.c) — see sd_menu.h.
bool cartridge_busy(void)
{
    return cfInserted != NONE || mdInUse;
}

// --- Cartridge hot-plug supervisor ---

//Tri-state one cartridge-facing pin: SIO input, no pulls. Nothing may drive
//a line toward the connector while the cartridge is absent or half-seated
//(back-powering through the display/SD clamp diodes).
static void cart_pin_tristate(uint pin)
{
    gpio_init(pin);
    gpio_disable_pulls(pin);
}

//Tri-state every cartridge-facing pin: display SPI, SD SPI, LED. The buttons
//keep their pull-ups (the schematic expects them, and a floating button line
//reads as ghost presses in the blocking menu loops); PIN_UI_DETECT keeps its
//pull-up. ISR-safe: pure per-pin register writes.
static void cart_pins_tristate(void)
{
    cart_pin_tristate(PIN_LED_ACTIVITY);
    cart_pin_tristate(SPI_TFT_CS);
    cart_pin_tristate(SPI_TFT_DC);
    cart_pin_tristate(UIEXT_TFT_SCK);
    cart_pin_tristate(UIEXT_TFT_MOSI);

    sd_card_t *sd = sd_get_by_num(0);
    cart_pin_tristate(sd->spi_if_p->spi->miso_gpio);
    cart_pin_tristate(sd->spi_if_p->spi->mosi_gpio);
    cart_pin_tristate(sd->spi_if_p->spi->sck_gpio);
    cart_pin_tristate(sd->spi_if_p->ss_gpio);
    //MISO keeps its pull-up (schematic-expected: the library enables it for
    //normal operation). Left floating it can read low, and 0x00 on DO means
    //"card present and busy" to the driver — sd_spi_test_com then reports
    //the card present forever and the menu loop never exits after a removal.
    //Pulled up, an absent card reads 0xFF and every driver loop times out.
    gpio_pull_up(sd->spi_if_p->spi->miso_gpio);
}

//Reconnect the cartridge-facing pins once presence is confirmed. The TFT
//pins are NOT restored here: setup_tft() reconfigures them from scratch when
//the state machine reaches INIT_SCREEN. The SD pins MUST be restored here —
//my_spi_init runs once per boot (spi_p->initialized) and re-running it would
//re-claim its DMA channels, so after a tri-state nobody else re-muxes them.
//Mirrors my_spi_init/sd_spi_ctor: SPI function + fast SCK slew + MISO
//pull-up, CS as SIO output idling high.
static void cart_pins_connect(void)
{
    gpio_init(PIN_LED_ACTIVITY);
    gpio_set_dir(PIN_LED_ACTIVITY, true);

    sd_card_t *sd = sd_get_by_num(0);
    //Full SPI0 block reset: a removal can tri-state the pins mid-transaction,
    //and a peripheral left with residual RX FIFO bytes shifts every later
    //response — the driver then retries forever against misaligned garbage.
    //my_spi_init runs once per boot, so nobody else ever resets it (the
    //display never hits this: setup_tft re-runs spi_init on ITS block every
    //time). 100 kHz mode-0 mirrors my_spi_init; the driver re-selects its
    //own baud rate per phase.
    spi_init(sd->spi_if_p->spi->hw_inst, 100 * 1000);
    gpio_set_function(sd->spi_if_p->spi->miso_gpio, GPIO_FUNC_SPI);
    gpio_set_function(sd->spi_if_p->spi->mosi_gpio, GPIO_FUNC_SPI);
    gpio_set_function(sd->spi_if_p->spi->sck_gpio, GPIO_FUNC_SPI);
    gpio_set_slew_rate(sd->spi_if_p->spi->sck_gpio, GPIO_SLEW_RATE_FAST);
    gpio_pull_up(sd->spi_if_p->spi->miso_gpio); //SD DO needs a pull-up
    gpio_init(sd->spi_if_p->ss_gpio);
    gpio_put(sd->spi_if_p->ss_gpio, 1);         //idle-high before output
    gpio_set_dir(sd->spi_if_p->ss_gpio, true);
    gpio_put(sd->spi_if_p->ss_gpio, 1);
}

//Immediate disconnect: tri-state everything, mark absent, re-arm the
//insertion debounce. Called from the detect IRQ and the SD error-burst path.
static void cart_disconnect(void)
{
    cart_pins_tristate();
    cartPresent = false;
    cartSdInvalid = true;
    cartReinitPending = true;
    cartLowSinceMs = 0;
}

//Removal confirm window. The detect line glitches — it always has (the
//updaters defend against "a PIN_UI_DETECT glitch"), and its finger neighbors
//SD MISO, so SPI edges couple into a high-Z line held by a weak pull-up. The
//old polled reads shrugged those spikes off; an edge IRQ latches every one,
//and each false disconnect tears down the whole UI session. So a high is
//only a removal if it SUSTAINS: sample for 2 ms and bail on the first low.
//A real removal keeps the line high forever (pull-up, cartridge gone), so
//reaction stays "within a few ms" as specified.
#define CART_REMOVE_CONFIRM_US 2000

static bool cart_detect_high_confirmed(void)
{
    uint64_t until = time_us_64() + CART_REMOVE_CONFIRM_US;
    while(time_us_64() < until)
        if(!gpio_get(PIN_UI_DETECT))
            return false;
    return true;
}

//Detect IRQ (raw handler, core 1): removal must tri-state within a few ms
//even while a blocking menu or a save loop runs, so it cannot wait for the
//polled state machine. Rising edge only — insertion is polled and debounced.
//The confirm spin runs at IRQ level: core 1 is UI-only, and it only happens
//on a rising edge (rare), so blocking this core's IRQs for 2 ms is fine.
static void cart_detect_irq(void)
{
    if(gpio_get_irq_event_mask(PIN_UI_DETECT) & GPIO_IRQ_EDGE_RISE)
    {
        gpio_acknowledge_irq(PIN_UI_DETECT, GPIO_IRQ_EDGE_RISE);
        if(cart_detect_high_confirmed())
            cart_disconnect();
    }
}

//Burst detector for FatFs operations on a mounted card. The stagger breaks
//detect LAST, so a rip-out cuts power/signals while detect may still read
//low for a moment — and a badly seated cartridge looks identical. Treat a
//run of consecutive SD failures as a probable surprise removal: tri-state
//and re-arm the debounce. If detect still reads low, the normal insertion
//sequence re-inits the display and remounts the card (recovering a reseat).
static void sd_io_result(bool ok)
{
    if(ok)
        sdErrStreak = 0;
    else if(++sdErrStreak >= SD_ERR_BURST_LIMIT)
        cart_disconnect();
}

//One-time hot-plug setup, on core 1 (the IRQ must fire on the UI core).
//Every cartridge-facing pin starts tri-stated: the cartridge may be absent
//or half-seated at power-on. Raw handler: coexists with the CYW43 driver's
//own raw GPIO handlers on the mainline board.
static void cart_hotplug_init(void)
{
    cart_pins_tristate();
    gpio_add_raw_irq_handler(PIN_UI_DETECT, cart_detect_irq);
    gpio_set_irq_enabled(PIN_UI_DETECT, GPIO_IRQ_EDGE_RISE, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

//Hot-plug supervisor tick, every UI loop pass — INCLUDING while the QL uses
//the drive (the mdInUse gate freezes only the menus, not removal safety).
static void cart_hotplug_task(void)
{
    //Poll-backup for the removal IRQ: a bounce edge can race the presence
    //commit below, ending with cartPresent set while the pins are already
    //tri-stated and the rising edge consumed — a state no future edge would
    //heal, since re-admission needs cartPresent false. Re-checking the raw
    //pin every pass makes any such state converge to a clean disconnect.
    //Same 2 ms confirm as the IRQ: a coupled spike is not a removal.
    if(cartPresent && gpio_get(PIN_UI_DETECT) && cart_detect_high_confirmed())
        cart_disconnect();

    if(cartSdInvalid)
    {
        //Thread-context cleanup after a disconnect. Tri-state again: the IRQ
        //may have raced a setup_tft/driver call that re-muxed a pin after
        //the ISR cleared it (task and UI code share core 1, so by now any
        //such call has finished). Then invalidate the SD driver state and
        //mark the volume unmounted — the card lost power before detect
        //broke, so both describe a dead card (same recipe as sd_fs_mount).
        cart_pins_tristate();
        sd_card_t *sd = sd_get_by_num(0);
        if(sd)
        {
            sd->state.m_Status |= STA_NOINIT;
            sd->state.card_type = SDCARD_NONE;
        }
        f_mount(NULL, "", 0);
        sdErrStreak = 0;
        cartSdInvalid = false;
    }

    if(cartPresent)
        return;

    //Insertion debounce: detect mates FIRST, so the first low edge says
    //nothing about VCC or the signal fingers. Require CART_DEBOUNCE_MS of
    //continuous low before driving anything toward the connector.
    if(gpio_get(PIN_UI_DETECT))
    {
        cartLowSinceMs = 0;
        return;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if(cartLowSinceMs == 0)
    {
        cartLowSinceMs = now ? now : 1;
        return;
    }
    if(now - cartLowSinceMs < CART_DEBOUNCE_MS)
        return;

    //Commit with IRQs off and the raw pin re-checked: a bounce edge right at
    //the debounce boundary could otherwise interleave the removal ISR with
    //this commit — ISR tri-states and clears presence, commit then sets it
    //back — leaving cartPresent true with dead pins and no future edge to
    //recover on. An edge arriving during the masked window stays latched in
    //the IO bank and the ISR runs a clean disconnect right after restore.
    uint32_t irq_state = save_and_disable_interrupts();
    bool admitted = !gpio_get(PIN_UI_DETECT);
    if(admitted)
    {
        cart_pins_connect();
        sdErrStreak = 0;
        cartPresent = true;
        //Display init + SD mount follow through the state machine as before:
        //IDLE → DELAY (500 ms panel settle) → INIT_SCREEN → WELCOME/mount.
    }
    else
        cartLowSinceMs = 0;
    restore_interrupts(irq_state);

    if(admitted)
    {
        //Insertion-accepted signature: one short blink the moment the
        //debounce admits the cartridge. User feedback, and a bench
        //diagnostic — a blink under a dead display separates "firmware
        //never re-admitted the cartridge" from "panel failed to re-init".
        LED_ON(PIN_LED_ACTIVITY);
        sleep_ms(100);
        LED_OFF(PIN_LED_ACTIVITY);
    }
}

//Current browse directory for thumb/sidecar path building (sandbox sd_menu
//API). With a cartridge inserted currentPath ends in the image file name â€”
//strip it to get back to the directory.
const char *sd_menu_cur_path(void)
{
    static char dirbuf[PATH_BUFFER_SIZE];

    strncpy(dirbuf, currentPath, PATH_BUFFER_SIZE - 1);
    dirbuf[PATH_BUFFER_SIZE - 1] = '\0';

    if(cfInserted != NONE)
    {
        char *lastPos = strrchr(dirbuf, '/');
        if(lastPos != NULL)
            *lastPos = '\0';
    }

    if(dirbuf[0] == '\0')
        strcpy(dirbuf, "/");

    return dirbuf;
}

static int cmp_dir_entry(const void *a, const void *b) {
    const DirEntry *da = a, *db = b;
    if (da->is_dir != db->is_dir) return da->is_dir ? -1 : 1;
    return strcmp(da->name, db->name);
}

//Writes a pair of buffers of a buffer set (a buffer set are four buffers, two header ones and two sector ones)
void write_buffer_set_pair(uint8_t* source, uint8_t* track1Buffer, uint8_t* track2Buffer, bool isHeader)
{
    track2Buffer += 4; //we skip four bits on buffer 2 to respect the skewing done by the ULA

    for(int buc = 0; buc < PREAMBLE_ZERO_BITS; buc++)
    {
        track1Buffer[buc] = 0;
        track2Buffer[buc] = 0;
    } 

    track1Buffer += PREAMBLE_ZERO_BITS;
    track2Buffer += PREAMBLE_ZERO_BITS;

    for(int buc = 0; buc < PREAMBLE_ONE_BITS; buc++)
    {
        track1Buffer[buc] = 1;
        track2Buffer[buc] = 1;
    } 

    track1Buffer += PREAMBLE_ONE_BITS;
    track2Buffer += PREAMBLE_ONE_BITS;

    uint16_t copySize = isHeader ? HEADER_TRACK_DATA_SIZE : SECTOR_TRACK_DATA_SIZE;

    for(int buc = 0; buc < copySize; buc++)
    {
        uint8_t t1b = *source;
        source++;
        uint8_t t2b = *source;
        source++;

        for(int buc = 0; buc < 8; buc++)
        {
            *track1Buffer = (t1b >> buc) & 1;
            *track2Buffer = (t2b >> buc) & 1;
            track1Buffer++;
            track2Buffer++;
        }
    }
}

//Writes a buffer set with cartridge data
void write_buffer_set(uint8_t setNumber, uint8_t sector)
{
    if(setNumber == 0)
    {
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector], header_1_track_1, header_1_track_2, true);
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector + CARTRIDGE_HEADER_SIZE], sector_1_track_1, sector_1_track_2, false);
        bufferset_1_sector_number = sector;
    }
    else
    {
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector], header_2_track_1, header_2_track_2, true);
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector + CARTRIDGE_HEADER_SIZE], sector_2_track_1, sector_2_track_2, false);
        bufferset_2_sector_number = sector;
    }
}

//Find the end of a preamble from a track buffer
int8_t find_preamble_end(uint8_t* buffer)
{
    uint8_t zeroCount = 0;
    uint8_t oneCount = 0;

    //We need to find at least 16 zeros followed by 8 ones (0x00, 0x00, 0xFF)
    for(int buc = 0; buc < 100; buc++)
    {
        if(buffer[buc] == 0)
        {
            if(oneCount != 0) //Do we came here form a one?
            {
                //Reset everything
                zeroCount = 1;
                oneCount = 0;
            }
            else
                zeroCount++; //Increment count

        }
        else
        {
            if(zeroCount < 16) //Did we found a one before having eight zeros?
            {
                //Reset everything
                oneCount = 0;
                zeroCount = 0;
            }
            else
                oneCount++; //Increment count
        }

        //Have we found the eight ones?
        if(oneCount == 8)
            return buc + 1;
    }

    //Error! We haven't found the gap end!!
    return -1;

}

bool inFormat = false;
int skip = 0;

//Reads a pair of buffers from a buffer set (a buffer set are four buffers, two header ones and two sector ones)
void read_buffer_set_pair(uint8_t* destination, uint8_t* track1Buffer, uint8_t* track2Buffer, bool isHeader)
{
    uint16_t track1Pos = find_preamble_end(track1Buffer);
    uint16_t track2Pos = find_preamble_end(track2Buffer);

    uint16_t size = isHeader ? HEADER_TRACK_DATA_SIZE : SECTOR_TRACK_DATA_SIZE;

    //Check if we're writting sector 255, if true then this is a format
    if(isHeader && !inFormat)
    {
        uint8_t sectorNumber = track2Buffer[track2Pos] |
            track2Buffer[track2Pos + 1] << 1 |
            track2Buffer[track2Pos + 2] << 2 |
            track2Buffer[track2Pos + 3] << 3 |
            track2Buffer[track2Pos + 4] << 4 |
            track2Buffer[track2Pos + 5] << 5 |
            track2Buffer[track2Pos + 6] << 6 |
            track2Buffer[track2Pos + 7] << 7;

        if(sectorNumber == 255)
        {
            inFormat = 1;
            skip = currentSector;
        }
    }

    for(uint16_t buc = 0; buc < size; buc++)
    {
        *destination = track1Buffer[track1Pos] |
            track1Buffer[track1Pos + 1] << 1 |
            track1Buffer[track1Pos + 2] << 2 |
            track1Buffer[track1Pos + 3] << 3 |
            track1Buffer[track1Pos + 4] << 4 |
            track1Buffer[track1Pos + 5] << 5 |
            track1Buffer[track1Pos + 6] << 6 |
            track1Buffer[track1Pos + 7] << 7;

        track1Pos += 8;
        destination++;

        *destination = track2Buffer[track2Pos] |
            track2Buffer[track2Pos + 1] << 1 |
            track2Buffer[track2Pos + 2] << 2 |
            track2Buffer[track2Pos + 3] << 3 |
            track2Buffer[track2Pos + 4] << 4 |
            track2Buffer[track2Pos + 5] << 5 |
            track2Buffer[track2Pos + 6] << 6 |
            track2Buffer[track2Pos + 7] << 7;

        track2Pos += 8;
        destination++;

    }
}

//Reads a buffer set to the cartridge buffer
void read_buffer_set(uint8_t setNumber)
{
    if(setNumber == 0)
    {
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_1_sector_number], header_1_track_1, header_1_track_2, true);
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_1_sector_number + CARTRIDGE_HEADER_SIZE], sector_1_track_1, sector_1_track_2, false);
    }
    else
    {
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_2_sector_number], header_2_track_1, header_2_track_2, true);
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_2_sector_number + CARTRIDGE_HEADER_SIZE], sector_2_track_1, sector_2_track_2, false);
    }
}

//Process when a buffer set has been read by the ULA
void process_md_read(uint8_t bufferSet)
{
    uint8_t secNum = cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 1];

    //If we are in the middle of a format and we're going to send sector 254, skip it to make Minerva happy...
    if(inFormat && secNum == 254)
    {
        currentSector++;

        if(currentSector > 253)
            currentSector = 0;
    }

    write_buffer_set(bufferSet, currentSector);

    //If we are in the middle of a format and we're going to send sector 13, damage it to make Minerva happy...
    if(inFormat && secNum == 13)
    {
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 13] += 13;
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 128] += 13;
    }

    currentSector++;

    if(currentSector == 255)
        currentSector = 0;
}

//Process when a buffer set has been written by the ULA
void process_md_write(uint8_t bufferSet)
{
    cartDirty = true; //RAM image now differs from the file on SD

    read_buffer_set(bufferSet);

    uint8_t secNum = cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 1];

    //If we are in the middle of a format and we're going to send sector 254, skip it to make Minerva happy...
    if(inFormat && secNum == 254)
    {
        currentSector++;

        if(currentSector > 253)
            currentSector = 0;
    }

    write_buffer_set(bufferSet, currentSector);

    //If we are in the middle of a format and we're going to send sector 13, damage it to make Minerva happy...
    if(inFormat && secNum == 13)
    {
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 13] += 13;
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 128] += 13;
    }

    currentSector++;
    if(currentSector == 255)
        currentSector = 0;
}

//Process events from the MD control
void process_md_to_ui_event(void* event)
{
    mtuevent_t* evt = (mtuevent_t*)event;

    switch(evt->event)
    {
        case MTU_MD_DESELECTED:

            mdInUse = false;
            inFormat = false;
            LED_OFF(PIN_LED_ACTIVITY);
            break;

        case MTU_MD_SELECTED:

            mdInUse = true;
            LED_ON(PIN_LED_ACTIVITY);
            break;

        case MTU_MD_READING:
        case MTU_MD_WRITTING:
            //Single activity LED, no read/write distinction anymore
            break;

        case MTU_BUFFERSET_READ:

            gpio_xor_mask(1u << PIN_LED_ACTIVITY); //Activity blip
            process_md_read(evt->arg);
            break;

        case MTU_BUFFERSET_WRITTEN:

            gpio_xor_mask(1u << PIN_LED_ACTIVITY); //Activity blip
            process_md_write(evt->arg);
            break;
    }
}

//Initialize the ST7735 SPI screen (RC auto-reset + SWRESET â€” cannot fail)
//Re-insertion is less forgiving than power-on: a quick hot swap can leave
//the panel's RC reset capacitor charged, so the panel powers up with NO
//hardware reset and a possibly desynced serial interface. Running the full
//init twice recovers it — the first pass's CS toggling resyncs the
//interface and issues SWRESET, the second lands on a properly reset panel.
//Boot keeps the single fast init (QL auto-boot race, UX review X15).
static bool screenInitBefore = false;

bool init_screen()
{
    if(screenInitBefore)
        setup_tft();
    setup_tft();
    screenInitBefore = true;
    return true;
}

//Centered error screen (sandbox status style, legacy message text)
//Title-less by design: the red title band is reserved for booting, System
//Tools and the path bar — plain status/error text stands alone.
static void ui_error(const char *text)
{
    uiext_wait_screen(text);
}

//Centered status screen (sandbox style, legacy message text)
static void ui_status(const char *text)
{
    uiext_wait_screen(text);
}

//Show the current file name to the screen

//Remove from the path buffer the last entry
void rewind_path()
{
    if(strlen(currentPath) == 0)
        return;

    char* lastPos = strrchr(currentPath, '/');

    if(lastPos == NULL)
        return;

    memset(lastPos, 0, (size_t)(PATH_BUFFER_SIZE - (lastPos - currentPath)));
}

void fix_cartridge_checksums()
{
    SECTOR_t* sector = (SECTOR_t*)cartridge_image;

    for(int buc = 0; buc < 255; buc++)
    {
        uint16_t computedChecksum = 0;

        for(int hBuc = 0; hBuc < 14; hBuc++)
            computedChecksum += sector->Header.HeaderData[hBuc];

        computedChecksum += 0x0f0f;
        sector->Header.Checksum = computedChecksum;

        computedChecksum = 0;

        for(int hrBuc = 0; hrBuc < 2; hrBuc++)
            computedChecksum += sector->Record.HeaderData[hrBuc];

        computedChecksum += 0x0f0f;
        sector->Record.HeaderChecksum = computedChecksum;

        computedChecksum = 0;

        for(int hdBuc = 0; hdBuc < 512; hdBuc++)
            computedChecksum += sector->Record.Data[hdBuc];

        computedChecksum += 0x0f0f;
        sector->Record.DataChecksum = computedChecksum;

        for (int bExtra = 0; bExtra < 84; bExtra++)
                sector->Record.ExtraBytes[bExtra] = bExtra % 2 == 0 ? 0xAA : 0x55;

        if (sector->Record.ExtraBytesChecksum != 0x3b19)
            sector->Record.ExtraBytesChecksum = 0x3b19;

        sector++;
    }
}

//Save the cartridge to a mdv image
//Animation tick for the blocking save loops (same dots12 cadence as the
//waiting screens). Uses the shared waitDots state, reset by the caller
//before the save starts.
static void tick_save_dots(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if(now >= waitDotsNextMs)
    {
        uiext_wait_anim(waitDots);
        waitDots = (waitDots + 1) % UIEXT_WAIT_ANIM_FRAMES;
        waitDotsNextMs = now + UIEXT_WAIT_ANIM_MS;
    }
}

bool save_mdv_cartridge()
{
    if(f_open(&fil, currentPath, FA_WRITE | FA_CREATE_ALWAYS))
        return false;

    UINT writeSize;
    uint8_t tmpByte = 0;
    int bufferPos = 0;

    uint8_t padBuffer[MDV_PAD_SIZE];
    memset(padBuffer, 'Z', MDV_PAD_SIZE);

    for(int buc = 0; buc < 255; buc++)
    {
        gpio_put(PIN_LED_ACTIVITY, (buc >> 2) & 1); //Fast blink while saving
        tick_save_dots();

        tmpByte = 0;

        for(int zeros = 0; zeros < PREAMBLE_ZERO_BYTES; zeros++)
        {
            if(f_write(&fil, &tmpByte, 1, &writeSize) || writeSize != 1)
            {
                f_close(&fil);
                return false;
            }
        }

        tmpByte = 0xff;

        for(int ones = 0; ones < PREAMBLE_ONE_BYTES; ones++)
        {
            if(f_write(&fil, &tmpByte, 1, &writeSize) || writeSize != 1)
            {
                f_close(&fil);
                return false;
            }
        }

        if(f_write(&fil, &cartridge_image[bufferPos], CARTRIDGE_HEADER_SIZE, &writeSize)
            || writeSize != CARTRIDGE_HEADER_SIZE)
        {
            f_close(&fil);
            return false;
        }

        bufferPos += CARTRIDGE_HEADER_SIZE;

        tmpByte = 0;

        for(int zeros = 0; zeros < PREAMBLE_ZERO_BYTES; zeros++)
        {
            if(f_write(&fil, &tmpByte, 1, &writeSize) || writeSize != 1)
            {
                f_close(&fil);
                return false;
            }
        }

        tmpByte = 0xff;

        for(int ones = 0; ones < PREAMBLE_ONE_BYTES; ones++)
        {
            if(f_write(&fil, &tmpByte, 1, &writeSize) || writeSize != 1)
            {
                f_close(&fil);
                return false;
            }
        }

        if(f_write(&fil, &cartridge_image[bufferPos], CARTRIDGE_DATA_SIZE, &writeSize)
            || writeSize != CARTRIDGE_DATA_SIZE)
        {
            f_close(&fil);
            return false;
        }

        bufferPos += CARTRIDGE_DATA_SIZE;

        if(f_write(&fil, padBuffer, MDV_PAD_SIZE, &writeSize) || writeSize != MDV_PAD_SIZE)
        {
            f_close(&fil);
            return false;
        }
    }

    return f_close(&fil) == FR_OK;
}

//Save the cartridge to a mpd image (chunked so the LED can blink)
bool save_mpd_cartridge()
{
    if(f_open(&fil, currentPath, FA_WRITE | FA_CREATE_ALWAYS))
        return false;

    UINT writeSize;
    UINT written = 0;

    while(written < CART_SIZE)
    {
        gpio_put(PIN_LED_ACTIVITY, (written >> 13) & 1); //Fast blink while saving
        tick_save_dots();

        UINT chunk = CART_SIZE - written;
        if(chunk > 8192)
            chunk = 8192;

        if(f_write(&fil, &cartridge_image[written], chunk, &writeSize) || writeSize != chunk)
        {
            f_close(&fil);
            return false;
        }

        written += chunk;
    }

    return f_close(&fil) == FR_OK;
}

//Load a MDV image to the cartridge buffer
bool load_mdv_cartridge()
{
    if(f_open(&fil, currentPath, FA_READ))
        return false;

    UINT readSize = 0;

    int bufferPos = 0;
    int filePos = 0;

    for(int buc = 0; buc < 255; buc++)
    {
        //Fast blink while loading. Time-based (not sector-based like the save
        //loops): a read is an order of magnitude quicker, so a per-sector
        //toggle would be too fast to see.
        gpio_put(PIN_LED_ACTIVITY, (to_ms_since_boot(get_absolute_time()) / 80) & 1);

        filePos = buc * MDV_SECTOR_SIZE + MDV_PREAMBLE_SIZE; //skip preamble

        if(f_lseek(&fil, filePos)
            || f_read(&fil, &cartridge_image[bufferPos], MDV_HEADER_SIZE, &readSize)
            || readSize != MDV_HEADER_SIZE)
        {
            f_close(&fil);
            return false;
        }

        filePos += MDV_HEADER_SIZE + MDV_PREAMBLE_SIZE;
        bufferPos += MPD_HEADER_SIZE;

        if(f_lseek(&fil, filePos)
            || f_read(&fil, &cartridge_image[bufferPos], MPD_DATA_SIZE, &readSize)
            || readSize != MPD_DATA_SIZE)
        {
            f_close(&fil);
            return false;
        }

        bufferPos += MPD_DATA_SIZE;
    }

    f_close(&fil);
    return true;
}

//Load a MPD image to the cartridge buffer (chunked so the LED can blink)
bool load_mpd_cartridge()
{
    if(f_open(&fil, currentPath, FA_READ))
        return false;

    UINT readSize = 0;
    UINT read = 0;

    while(read < CART_MPD_SIZE)
    {
        gpio_put(PIN_LED_ACTIVITY, (to_ms_since_boot(get_absolute_time()) / 80) & 1);

        UINT chunk = CART_MPD_SIZE - read;
        if(chunk > 8192)
            chunk = 8192;

        if(f_read(&fil, &cartridge_image[read], chunk, &readSize) || readSize != chunk)
        {
            f_close(&fil);
            return false;
        }

        read += chunk;
    }

    f_close(&fil);
    return true;

}

//Clear the sticky overflow flags of all the event machines. Only allowed on
//eject: the stale cartridge image is discarded so the lost events no longer
//matter.
static void clear_event_overflows(void)
{
    mdEventQueue.overflow = false;
    uiToMdEventQueue.overflow = false;
    mdToUiEventQueue.overflow = false;
}

//Builds the menu item list from dir_entries: items[0] = unused header slot
//(band renderer convention), then "[..]" below root, then the entries â€”
//"[dir]" bracketed, files starred when they match the CONFIG.CFG tag.
static void build_menu_labels(void)
{
    disp_names[0][0] = '\0';
    disp_ptrs[0] = disp_names[0];
    item_count = 1;

    if(has_up)
    {
        strcpy(disp_names[1], "[..]");
        disp_ptrs[1] = disp_names[1];
        item_count = 2;
    }

    for(int i = 0; i < dir_count; i++)
    {
        char *dst = disp_names[item_count];
        char fullpath[PATH_BUFFER_SIZE];
        snprintf(fullpath, PATH_BUFFER_SIZE, "%s/%s",
                 currentPath, dir_entries[i].name);

        if(dir_entries[i].is_dir)
            snprintf(dst, MAX_NAME_LEN + 3, "[%s]", dir_entries[i].name);
        else if(configTaggedPath[0] != '\0' &&
                strcmp(fullpath, configTaggedPath) == 0)
            snprintf(dst, MAX_NAME_LEN + 3, "*%s", dir_entries[i].name);
        else
            snprintf(dst, MAX_NAME_LEN + 3, "%s", dir_entries[i].name);

        //Menu items display in uppercase (labels are display-only; file
        //operations keep dir_entries[].name as stored on card).
        for(char *p = dst; *p; p++)
            if(*p >= 'a' && *p <= 'z')
                *p -= 'a' - 'A';

        disp_ptrs[item_count] = dst;
        item_count++;
    }

    //Scan hit the listing cap with entries left over: close the list with a
    //non-selectable "..." marker so the truncation is visible.
    if(dir_truncated)
    {
        strcpy(disp_names[item_count], "...");
        disp_ptrs[item_count] = disp_names[item_count];
        item_count++;
    }
}

void program_delay(uint64_t ms_delay, USER_INTERFACE_STATE nextState)
{
    delayEnd = time_us_64() + (ms_delay * 1000);
    uiNextState = nextState;
    uiState = DELAY;
}

void check_delay()
{
    uint64_t currentTime = time_us_64();

    if(currentTime >= delayEnd)
        uiState = uiNextState;
}

//Process the user interface state machine
void process_user_interface()
{
    //Replug after a disconnect (removal IRQ or SD error burst): restart from
    //IDLE so INIT_SCREEN re-runs the panel init — mandatory after the panel
    //lost power, and the machine may have been stuck inside a blocking menu
    //when the removal happened, past its own IS_UI_DISCONNECTED checks.
    if(cartReinitPending && !IS_UI_DISCONNECTED())
    {
        cartReinitPending = false;
        uiState = IDLE;
    }

    switch(uiState)
    {
        case IDLE:

            //500 ms settle before touching the display (panel RC auto-reset
            //needs ~150 ms after power; 3x margin). Was 2000 ms — shortened
            //for the QL auto-boot race (UX review X15).
            if(!IS_UI_DISCONNECTED())
                program_delay(500, INIT_SCREEN);

            break;

        case DELAY:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
                check_delay();

            break;

        case INIT_SCREEN:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                if(init_screen())
                {
                    //Alternative update path: firmware dropped in SD:/.update —
                    //display is up, cartridge not loaded yet, so the flash
                    //writes are safe. No-op without SD/folder. UNCONDITIONAL:
                    //both boards have an SD updater (different flavours, see
                    //sd_update.h). This call used to sit inside the OTA gate
                    //below, which would have removed SD updating from the
                    //RP2040 image entirely — study trap T2.
                    sd_update_check_at_boot();

#if UIEXT_OTA_ENABLED
                    //App-initiated reboot (§5 "reboot" op): come back up
                    //listening so the app can reconnect straight away —
                    //skip the boot animation. Scratch survives the watchdog
                    //reset, is cleared on power-on and consumed here.
                    if(watchdog_hw->scratch[0] == OTA_REBOOT_TO_CONNECT_MAGIC)
                    {
                        watchdog_hw->scratch[0] = 0;
                        ota_run_connect_mode();
                        (void)ota_sd_changed(); //listing gets built fresh below
                        uiState = SHOW_WAITING_SD_CARD;
                    }
                    else
#endif
                    uiState = WELCOME;
                }
            }
            break;

        case WELCOME:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                //Boot screen with the braille spinner (same as the Lite
                //branch; replaced the GIF spinner and its frame buffer).
                //The title bar already names the device, so the caption
                //line carries the action + firmware version.
                char boot_caption[24];
                snprintf(boot_caption, sizeof(boot_caption), "Loading v%d.%d.%d",
                         UIEXT_FW_VERSION_MAJOR, UIEXT_FW_VERSION_MINOR,
                         UIEXT_FW_VERSION_PATCH);
                uiext_ota_wait("MicroPicoDrive", boot_caption);
                uiext_boot_spinner(0);

                if(cfInserted == NONE)
                    memset(currentPath, 0, PATH_BUFFER_SIZE);

                //QL auto-boot race (UX review X15, hybrid per owner): the QL
                //polls mdv1 for a boot cartridge within seconds of power-on,
                //so the first mount + CONFIG.CFG autoload run UNDER the
                //spinner. Only the tagged-autoload path skips the remaining
                //animation — that is the only path with a race. Untagged
                //boots have nothing to serve, so they keep the full boot
                //animation before opening the browser.
                if(cfInserted == NONE && mount_sd() && try_config_autoload())
                {
                    //Autoload set CARTRIDGE_READY and drew its screen —
                    //serving as early as possible.
                }
                else
                {
                    //No race here: finish the spinner animation, then either
                    //browse (card mounted, nothing tagged / autoload failed)
                    //or wait for a card.
                    for(int frame = 1; frame < 20; frame++)
                    {
                        uiext_boot_spinner(frame);
                        sleep_ms(80);  //cli-spinners "dots" cadence
                    }

                    if(cfInserted == NONE && mount_sd())
                        uiState = OPEN_FOLDER;
                    else
                        uiState = SHOW_WAITING_SD_CARD;
                }
            }
            break;

        case SHOW_WAITING_SD_CARD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                //No K4 hint here (owner 2026-07-31): the No-MDV-files screen
                //keeps it — that is where a stuck user needs Tools.
                uiext_wait_anim_screen("Waiting Card");
                waitDots = 0;
                waitDotsNextMs = 0;
                uiState = WAITING_SD_CARD;
            }
            break;

        case WAITING_SD_CARD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else if(BUTTON_PRESSED(PIN_BTN_SYSTEM))
            {
                debounce_button(PIN_BTN_SYSTEM);
                uiext_system_tools();
                uiState = SHOW_WAITING_SD_CARD; //redraw the wait chrome
            }
            else
            {
                uint32_t now = to_ms_since_boot(get_absolute_time());

                if(now >= waitDotsNextMs)
                {
                    uiext_wait_anim(waitDots);
                    //SD probe stays at the old ~400 ms cadence (every 5th
                    //frame) — a full mount attempt per 80 ms frame is waste.
                    bool probe = (waitDots % 5) == 0;
                    waitDots = (waitDots + 1) % UIEXT_WAIT_ANIM_FRAMES;
                    waitDotsNextMs = now + UIEXT_WAIT_ANIM_MS;

                    if(probe && mount_sd())
                    {
                        if(cfInserted == NONE)
                        {
                            if(!try_config_autoload())
                                uiState = OPEN_FOLDER;
                        }
                        else
                        {
                            uiState = CARTRIDGE_READY;
                            show_cart_ready();
                        }
                    }
                }
            }

            break;

        case OPEN_FOLDER:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                if(f_opendir(&dir, currentPath))
                {
                    sd_io_result(false);
                    ui_error("Open failed");
                    sleep_ms(2000);
                    uiState = SHOW_WAITING_SD_CARD;
                }
                else
                {
                    dir_count = 0;
                    while(dir_count < MAX_DIR_ITEMS)
                    {
                        if(f_readdir(&dir, &fno))
                        {
                            sd_io_result(false);
                            ui_error("Read failed");
                            sleep_ms(2000);
                            uiState = SHOW_WAITING_SD_CARD;
                            break;
                        }
                        if(fno.fname[0] == 0) break;
                        if(fno.fattrib & (AM_HID | AM_SYS)) continue;
                        if(fno.fname[0] == '.') continue; //hide dot entries (.update etc.)
                        if(!(fno.fattrib & AM_DIR))
                        {
                            const char *ext = strrchr(fno.fname, '.');
                            if(!ext ||
                               (strcasecmp(ext, ".MDV") != 0 && strcasecmp(ext, ".MPD") != 0))
                                continue;
                        }
                        snprintf(dir_entries[dir_count].name, MAX_NAME_LEN, "%s", fno.fname);
                        dir_entries[dir_count].fsize  = fno.fsize;
                        dir_entries[dir_count].is_dir = (fno.fattrib & AM_DIR) != 0;
                        dir_count++;
                    }

                    //Cap hit: peek whether a storable entry remains, so the
                    //list can end with the "..." truncation marker.
                    dir_truncated = false;
                    if(uiState != SHOW_WAITING_SD_CARD && dir_count == MAX_DIR_ITEMS)
                    {
                        while(!f_readdir(&dir, &fno) && fno.fname[0])
                        {
                            if(fno.fattrib & (AM_HID | AM_SYS)) continue;
                            if(fno.fname[0] == '.') continue;
                            if(!(fno.fattrib & AM_DIR))
                            {
                                const char *ext = strrchr(fno.fname, '.');
                                if(!ext ||
                                   (strcasecmp(ext, ".MDV") != 0 && strcasecmp(ext, ".MPD") != 0))
                                    continue;
                            }
                            dir_truncated = true;
                            break;
                        }
                    }
                    f_closedir(&dir);

                    if(uiState != SHOW_WAITING_SD_CARD)
                    {
                        sd_io_result(true);
                        qsort(dir_entries, dir_count, sizeof(DirEntry), cmp_dir_entry);

                        has_up = (strlen(currentPath) > 0);
                        build_menu_labels();

                        menu_offset = 0;
                        uiState = BROWSE_FOLDER;
                    }
                }
            }

            break;

        case BROWSE_FOLDER:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                bool not_at_root = (strlen(currentPath) > 0);

                if(dir_count == 0)
                {
                    if(not_at_root)
                    {
                        ui_error("Empty dir");
                        sleep_ms(2000);
                        rewind_path();
                        menu_offset = 0;
                        uiState = OPEN_FOLDER;
                    }
                    else
                        //Nothing loadable at root (fresh or non-MDV card):
                        //a persistent wait screen instead of an error loop.
                        uiState = SHOW_NO_FILES;
                }
                else
                {
                    int sel = uiext_menu_run(disp_ptrs, item_count, &menu_offset);

                    if(sel == UIEXT_MENU_SD_GONE)
                    {
                        //Card pulled while the listing was up: it describes a
                        //card that is no longer there.
                        memset(currentPath, 0, PATH_BUFFER_SIZE);
                        menu_offset = 0;
                        uiState = SHOW_WAITING_SD_CARD;
                    }
                    else if(sel == UIEXT_MENU_SD_CHANGED)
                    {
                        //A BLE Connect session changed the SD — rebuild the
                        //now-stale listing (currentPath stays valid: Connect
                        //deletes files only, never directories).
                        menu_offset = 0;
                        uiState = OPEN_FOLDER;
                    }
                    else if(sel == UIEXT_LONG_SELECT)
                    {
                        // Long-press SELECT: toggle auto-load tag on the
                        // highlighted file (the engine already toggled the '*'
                        // preview on the label; rebuilding the labels below
                        // normalizes it either way).
                        int entry_idx = menu_offset - (has_up ? 1 : 0);
                        if(entry_idx >= 0 && entry_idx < dir_count &&
                           !dir_entries[entry_idx].is_dir)
                        {
                            char fullpath[PATH_BUFFER_SIZE];
                            snprintf(fullpath, PATH_BUFFER_SIZE, "%s/%s",
                                     currentPath, dir_entries[entry_idx].name);

                            bool already_tagged = (configTaggedPath[0] != '\0' &&
                                                   strcmp(fullpath, configTaggedPath) == 0);
                            write_config_tag(already_tagged ? "" : fullpath);
                        }
                        build_menu_labels();
                        uiext_menu_draw(disp_ptrs, item_count, menu_offset);
                        // stay in BROWSE_FOLDER â€” no uiState change
                    }
                    else if(sel > 0)
                    {
                        int entry_idx = sel - 1 - (has_up ? 1 : 0);

                        if(entry_idx < 0)
                        {
                            // "[..]" â€” directory up
                            rewind_path();
                            menu_offset = 0;
                            uiState = OPEN_FOLDER;
                        }
                        else if(entry_idx >= dir_count)
                        {
                            // "..." truncation marker — not selectable
                        }
                        else if(dir_entries[entry_idx].is_dir)
                        {
                            CONCAT(currentPath, dir_entries[entry_idx].name);
                            menu_offset = 0;
                            uiState = OPEN_FOLDER;
                        }
                        else
                        {
                            selected_fsize = dir_entries[entry_idx].fsize;
                            snprintf(selected_name, MAX_NAME_LEN, "%s", dir_entries[entry_idx].name);
                            uiState = FILE_SELECTED;
                        }
                    }
                }
            }

            break;

        case SHOW_NO_FILES:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                //Centered caption+animation pair, same as Waiting Card (the
                //K4 hint was dropped here too — owner 2026-07-31).
                uiext_wait_anim_screen("No MDV files");
                waitDots = 0;
                waitDotsNextMs = 0;
                uiState = NO_FILES;
            }
            break;

        case NO_FILES:

            //Same chrome/cadence as the Waiting-SD screen; leaves when the
            //card is pulled (to add files) or via System Tools.
            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else if(BUTTON_PRESSED(PIN_BTN_SYSTEM))
            {
                debounce_button(PIN_BTN_SYSTEM);
                uiext_system_tools();
                uiState = SHOW_NO_FILES;
            }
            else
            {
                uint32_t now = to_ms_since_boot(get_absolute_time());

                if(now >= waitDotsNextMs)
                {
                    uiext_wait_anim(waitDots);
                    //Card-pulled probe at the old ~400 ms cadence (see the
                    //Waiting-Card loop).
                    bool probe = (waitDots % 5) == 0;
                    waitDots = (waitDots + 1) % UIEXT_WAIT_ANIM_FRAMES;
                    waitDotsNextMs = now + UIEXT_WAIT_ANIM_MS;

                    if(probe && !mount_sd())
                        uiState = SHOW_WAITING_SD_CARD;
                }
            }
            break;

        case FILE_SELECTED:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                switch(selected_fsize)
                {

                    case CART_MDV_SIZE:

                        // Box-art screen with the legacy load text as caption
                        uiext_cart_screen(selected_name, "Load MDV");
                        cfInserted = MDV;
                        uiState = FILE_LOAD;
                        break;

                    case CART_MPD_SIZE:

                        uiext_cart_screen(selected_name, "Load MPD");
                        cfInserted = MPD;
                        uiState = FILE_LOAD;
                        break;

                    default:

                        ui_error("Bad format");
                        sleep_ms(2000);
                        uiState = BROWSE_FOLDER;
                        break;

                }
            }

            break;

        case FILE_LOAD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                CONCAT(currentPath, selected_name);

                bool res = false;

                switch(cfInserted)
                {
                    case MDV:
                        res = load_mdv_cartridge();
                        break;
                    case MPD:
                        res = load_mpd_cartridge();
                        break;
                }

                LED_OFF(PIN_LED_ACTIVITY);
                sd_io_result(res);

                if(!res)
                {
                    ui_error("Load failed");
                    rewind_path();
                    sleep_ms(2000);
                    cfInserted = NONE;
                    uiState = OPEN_FOLDER;
                }
                else
                {
                    ui_status("Validating");

                    fix_cartridge_checksums();

                    write_buffer_set(0, 0);
                    write_buffer_set(1, 1);
                    currentSector = 2;
                    cartDirty = false;
                    //Remember which card the image came from (save guard)
                    if(f_getlabel("", NULL, &cartCardSerial) != FR_OK)
                        cartCardSerial = 0;
                    uiState = CARTRIDGE_READY;
                    utmevent_t insertEvt;
                    insertEvt.event = UTM_CARTRIDGE_INSERTED;
                    event_push(&uiToMdEventQueue, &insertEvt);
                    show_cart_ready();
                }
            }

            break;

        case CARTRIDGE_READY:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                if(BUTTON_PRESSED(PIN_BTN_BACK) || BUTTON_PRESSED(PIN_BTN_NEXT))
                {
                    //Either navigation key (UP or DOWN) ejects back to the list.
                    debounce_button(BUTTON_PRESSED(PIN_BTN_BACK) ? PIN_BTN_BACK
                                                                 : PIN_BTN_NEXT);

                    // Dirty-flag eject: instant when clean, centered Yes/No
                    // confirm when the QL wrote sectors since last load/save.
                    bool eject = true;
                    if(cartDirty)
                    {
                        eject = uiext_ota_confirm_centered("Eject?", "Unsaved changes",
                                                           UIEXT_CONFIRM_TIMEOUT_MS);
                        if(!eject)
                            show_cart_ready();
                    }

                    if(eject)
                    {
                        rewind_path();
                        uiState = OPEN_FOLDER;
                        cfInserted = NONE;
                        cartDirty = false;
                        utmevent_t removeEvt;
                        removeEvt.event = UTM_CARTRIDGE_REMOVED;
                        clear_event_overflows();
                        event_push(&uiToMdEventQueue, &removeEvt);
                    }
                }
                else if(BUTTON_PRESSED(PIN_BTN_SELECT))
                {
                    debounce_button(PIN_BTN_SELECT);

                    //Card-swap save guard (UX review X16): the save opens
                    //currentPath with FA_CREATE_ALWAYS, so a card swapped
                    //mid-session would get a file created/overwritten at the
                    //old card's path. Fresh mount + volume-serial compare;
                    //on mismatch the save is refused and the RAM image plus
                    //dirty flag stay untouched (guard skipped when either
                    //serial is unknown — a failed mount then fails the save
                    //itself, keeping today's behavior).
                    DWORD vsn = 0;
                    if(!mount_sd() || f_getlabel("", NULL, &vsn) != FR_OK)
                        vsn = 0;
                    if(cartCardSerial != 0 && vsn != 0 && vsn != cartCardSerial)
                    {
                        uiext_wait_screen2("Different card", "Save blocked");
                        sleep_ms(2000);
                        show_cart_ready();
                        break;
                    }

                    //The save takes seconds: title-less caption+animation
                    //pair, ticked inside the save loops.
                    uiext_wait_anim_screen("Saving");
                    waitDots = 0;
                    waitDotsNextMs = 0;

                    bool res = false;

                    switch(cfInserted)
                    {
                        case MDV:
                            res = save_mdv_cartridge();
                            break;
                        case MPD:
                            res = save_mpd_cartridge();
                            break;
                    }

                    LED_OFF(PIN_LED_ACTIVITY);
                    //Removal mid-save lands here as a failed save: the RAM
                    //image and the dirty flag are kept (same guard as a
                    //power failure), and the burst detector sees the error.
                    sd_io_result(res);

                    if(res)
                    {
                        cartDirty = false;
                        //Serial unknown at load time: adopt the card that
                        //now holds the file as the image's home.
                        if(cartCardSerial == 0)
                            f_getlabel("", NULL, &cartCardSerial);
                        ui_status("Saved");
                        sleep_ms(2000);
                        show_cart_ready();
                    }
                    else
                    {
                        //FA_CREATE_ALWAYS truncates before writing: a failed
                        //save leaves the on-card file incomplete, so the RAM
                        //image (kept, still dirty) must be saved again.
                        uiext_wait_screen2("Save failed", "Retry save!");
                        sleep_ms(2000);
                        show_cart_ready();
                    }
                }
                else if(BUTTON_PRESSED(PIN_BTN_SYSTEM))
                {
                    debounce_button(PIN_BTN_SYSTEM);

                    //System Tools is a blocking menu: with a cartridge
                    //mounted it starves the 16-deep MD event queues (ends in
                    //EVENT_LOST + forced eject) and settings saves are gated
                    //off anyway. Same policy as the mainline radio modes:
                    //tools require the cartridge ejected.
                    ui_status("Eject first");
                    sleep_ms(1500);
                    show_cart_ready();
                }
            }

            break;

        case EVENT_LOST:

            //Events were lost, the cartridge image is untrusted: SAVE is
            //unreachable from here and the only exit is BACK (eject).
            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else if(BUTTON_PRESSED(PIN_BTN_BACK))
            {
                debounce_button(PIN_BTN_BACK);
                rewind_path();
                cfInserted = NONE;
                cartDirty = false;
                currentSector = 0;
                utmevent_t removeEvt;
                removeEvt.event = UTM_CARTRIDGE_REMOVED;
                clear_event_overflows();
                event_push(&uiToMdEventQueue, &removeEvt);
                uiState = OPEN_FOLDER;
            }

            break;

    }
}

//Initialize UI buttons
void init_buttons()
{
    gpio_init(PIN_BTN_BACK);
    gpio_init(PIN_BTN_NEXT);
    gpio_init(PIN_BTN_SELECT);
    gpio_init(PIN_BTN_SYSTEM);
    gpio_init(PIN_UI_DETECT);

    gpio_set_dir(PIN_BTN_BACK, false);
    gpio_set_dir(PIN_BTN_NEXT, false);
    gpio_set_dir(PIN_BTN_SELECT, false);
    gpio_set_dir(PIN_BTN_SYSTEM, false);
    gpio_set_dir(PIN_UI_DETECT, false);

    gpio_pull_up(PIN_BTN_BACK);
    gpio_pull_up(PIN_BTN_NEXT);
    gpio_pull_up(PIN_BTN_SELECT);
    gpio_pull_up(PIN_BTN_SYSTEM);
    gpio_pull_up(PIN_UI_DETECT);
}

//Slow ~1 Hz blink on the waiting/error screens; MD events drive the LED in
//the other states (solid on select, toggle per buffer set).
static void led_task(void)
{
    if(uiState == WAITING_SD_CARD || uiState == SHOW_WAITING_SD_CARD
        || uiState == EVENT_LOST)
        gpio_put(PIN_LED_ACTIVITY, (to_ms_since_boot(get_absolute_time()) / 500) & 1);
}

// Cartridge-ready screen: box-art thumb sidecar ("Mounted" + name block
// otherwise) captioned with the image name (no extension, pixel-clipped
// with .. if wide — the old 18-char cap let wide names overflow).
static void show_cart_ready(void) {
    char base[MAX_NAME_LEN];
    strncpy(base, selected_name, MAX_NAME_LEN - 1);
    base[MAX_NAME_LEN - 1] = '\0';
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
    uiext_pixel_clip(label, base, UIEXT_DISPLAY_WIDTH);
    uiext_cart_screen(selected_name, label);
}

// Reads CONFIG.CFG from SD root, populates configTaggedPath with the FILE= value.
// Returns true if a non-empty FILE= value was found, false otherwise.
// An absent file is simply "no tag" — the star-tag flow creates it on demand
// (write_config_tag opens with FA_CREATE_ALWAYS), so an untagged card is
// never written to just for being mounted.
static bool read_config_file(void) {
    char buf[CONFIG_FILE_SIZE + 1];
    UINT br = 0;

    configTaggedPath[0] = '\0';

    if (f_open(&fil, "CONFIG.CFG", FA_READ))
        return false;

    if (f_read(&fil, buf, CONFIG_FILE_SIZE, &br)) {
        f_close(&fil);
        return false;
    }

    f_close(&fil);
    buf[br] = '\0';

    char *p = strstr(buf, "FILE=");
    if (!p)
        return false;

    p += 5;

    char *end = p;
    while (*end && *end != '\n' && *end != '\r')
        end++;
    while (end > p && *(end - 1) == ' ')
        end--;

    int len = (int)(end - p);
    if (len <= 0)
        return false;

    int copy = (len < PATH_BUFFER_SIZE - 1) ? len : PATH_BUFFER_SIZE - 1;
    strncpy(configTaggedPath, p, copy);
    configTaggedPath[copy] = '\0';
    return true;
}

// Reads CONFIG.CFG and, if a valid FILE= path is found, loads the image and
// transitions to CARTRIDGE_READY. Returns true on successful auto-load.
static bool try_config_autoload(void) {
    if (!read_config_file())
        return false;

    const char *dot = strrchr(configTaggedPath, '.');
    if (!dot)
        return false;

    CARTRIDGE_FORMAT fmt;
    if (strcmp(dot, ".MDV") == 0 || strcmp(dot, ".mdv") == 0)
        fmt = MDV;
    else if (strcmp(dot, ".MPD") == 0 || strcmp(dot, ".mpd") == 0)
        fmt = MPD;
    else
        return false;

    cfInserted = fmt;
    strncpy(currentPath, configTaggedPath, PATH_BUFFER_SIZE - 1);
    currentPath[PATH_BUFFER_SIZE - 1] = '\0';
    const char *slash = strrchr(configTaggedPath, '/');
    const char *base_name = slash ? slash + 1 : configTaggedPath;
    snprintf(selected_name, MAX_NAME_LEN, "%s", base_name);

    bool res = (fmt == MDV) ? load_mdv_cartridge() : load_mpd_cartridge();
    LED_OFF(PIN_LED_ACTIVITY);
    if (!res) {
        ui_error("Load failed");
        rewind_path();
        cfInserted = NONE;
        sleep_ms(2000);
        return false;
    }

    fix_cartridge_checksums();
    write_buffer_set(0, 0);
    write_buffer_set(1, 1);
    currentSector = 2;
    cartDirty = false;
    //Remember which card the image came from (save guard)
    if (f_getlabel("", NULL, &cartCardSerial) != FR_OK)
        cartCardSerial = 0;
    uiState = CARTRIDGE_READY;

    utmevent_t evt;
    evt.event = UTM_CARTRIDGE_INSERTED;
    event_push(&uiToMdEventQueue, &evt);

    show_cart_ready();
    return true;
}

// Writes the given full_path as FILE= into CONFIG.CFG (fixed-size), creating
// the file when absent.
static bool write_config_tag(const char *full_path) {
    char buf[CONFIG_FILE_SIZE];
    UINT bw = 0;

    memset(buf, ' ', CONFIG_FILE_SIZE);
    buf[0] = 'F'; buf[1] = 'I'; buf[2] = 'L'; buf[3] = 'E'; buf[4] = '=';
    int plen = (int)strlen(full_path);
    if (plen > 300) plen = 300;
    memcpy(buf + 5, full_path, (size_t)plen);
    buf[CONFIG_FILE_SIZE - 1] = '\n';

    if (f_open(&fil, "CONFIG.CFG", FA_WRITE | FA_CREATE_ALWAYS)) {
        ui_error("Write failed");
        sleep_ms(2000);
        return false;
    }

    if (f_write(&fil, buf, CONFIG_FILE_SIZE, &bw) || bw != CONFIG_FILE_SIZE) {
        f_close(&fil);
        ui_error("Write failed");
        sleep_ms(2000);
        return false;
    }

    if (f_close(&fil) != FR_OK) {
        ui_error("Write failed");
        sleep_ms(2000);
        return false;
    }

    strncpy(configTaggedPath, full_path, PATH_BUFFER_SIZE - 1);
    configTaggedPath[PATH_BUFFER_SIZE - 1] = '\0';
    return true;
}

//Display settings persist on BOTH boards, in different places — study trap
//T1. This whole block (and both accessors below) used to sit behind
//#if UIEXT_OTA_ENABLED, i.e. the RADIO flag silently governed whether the
//feature existed at all. Folding mainline's file as-is would have produced an
//RP2040 image whose config_save_settings() was an empty function: Caption,
//Theme, Path Bar and Rainbow resetting on every power cycle, with no error.
//The real condition is "this build persists settings", which is both boards.
//
//The two backends are NOT interchangeable: 0x344000 is past the end of the
//RP2040's 2 MB flash, and XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE is RP2350-only.
#if UIEXT_SETTINGS_PERSIST
#define SETTINGS_MAGIC 0x4D504453u  //"MPDS"

#if UIEXT_OTA_ENABLED
//Third sector of the "data" partition (physical 0x344000 — the BTstack TLV
//bond banks own its first 8 KB, see ota_flash_bank.c). Reads use the
//untranslated flash view: under an A/B partition boot, XIP_BASE only maps the
//booted slot (QMI translation) and absolute-offset reads through it hang.
#define SETTINGS_FLASH_OFFSET 0x344000u  //keep in sync with ota_flash_bank.c
#define SETTINGS_XIP_BASE     XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#else
//Last sector of the 2 MB part, below the staging region (flash_layout.h,
//which _Static_asserts the map). No partition table, so the plain XIP view is
//correct here.
#define SETTINGS_FLASH_OFFSET FL_SETTINGS_OFFSET
#define SETTINGS_XIP_BASE     XIP_BASE
#endif

//Erased flash reads 0xFF so a missing magic means "never saved" and the
//defaults stay.
typedef struct {
    uint32_t magic;
    uint8_t  theme_dark;
    uint8_t  caption;
    uint8_t  pathbar;
    uint8_t  rainbow;
} settings_rec_t;

static void settings_flash_write_cb(void *param)
{
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, (const uint8_t *)param, FLASH_PAGE_SIZE);
}
#endif

//Persist the System Tools display settings to flash. Flash writes stall XIP
//for both cores, so — same hard rule as the radio modes — only with the
//cartridge ejected and the QL not using the drive; otherwise the changes
//stay for the session only.
void config_save_settings(void)
{
#if UIEXT_SETTINGS_PERSIST
    if(cfInserted != NONE || mdInUse)
        return;

    static uint8_t page[FLASH_PAGE_SIZE];  //program granularity
    memset(page, 0xFF, sizeof(page));
    settings_rec_t rec = {
        .magic      = SETTINGS_MAGIC,
        .theme_dark = uiext_theme_is_dark() ? 1 : 0,
        .caption    = (uint8_t)g_caption_pos,
        .pathbar    = g_path_title_enabled ? 1 : 0,
        .rainbow    = (uint8_t)uiext_rainbow_get(),
    };
    memcpy(page, &rec, sizeof(rec));

    if(flash_safe_execute(settings_flash_write_cb, page, 100) != PICO_OK)
    {
        ui_error("Save failed");
        sleep_ms(2000);
    }
#endif
}

//Apply the flash-persisted settings (no-op on erased flash / foreign magic).
static void config_load_settings(void)
{
#if UIEXT_SETTINGS_PERSIST
    const settings_rec_t *rec = (const settings_rec_t *)
        (SETTINGS_XIP_BASE + SETTINGS_FLASH_OFFSET);

    if(rec->magic != SETTINGS_MAGIC)
        return;

    uiext_theme_set_dark(rec->theme_dark != 0);
    if(rec->caption < CAPTION_POS_COUNT)
        g_caption_pos = (caption_pos_t)rec->caption;
    g_path_title_enabled = (rec->pathbar != 0);
    uiext_rainbow_set(rec->rainbow);  //out-of-range = ignored
#endif
}

//Main user interface loop
void RunUserInterface()
{
    config_load_settings();

    event_machine_init(&mdToUiEventQueue, &process_md_to_ui_event, sizeof(mtuevent_t), 16);
    mtuevent_t mtuevtBuffer;

    init_buttons();     //buttons + PIN_UI_DETECT pull-up, before the IRQ arms
    cart_hotplug_init(); //tri-state cartridge pins (LED included), arm detect IRQ

    while(true)
    {
        //Hot-plug supervisor first: it must run even while the QL is using
        //the drive (the mdInUse gate below freezes only the menus).
        cart_hotplug_task();

        event_process_queue(&mdToUiEventQueue, &mtuevtBuffer, 16);

        //A sticky overflow means events were lost. With a cartridge inserted
        //the RAM image can no longer be trusted: force the error screen
        //(deferred while the screen is not initialized — the flag is sticky).
        //Without a cartridge there is nothing to corrupt — the QL polls its
        //drives even with none loaded, flooding the queue while the blocking
        //menu runs — so the flags are cleared quietly.
        if(mdEventQueue.overflow || uiToMdEventQueue.overflow || mdToUiEventQueue.overflow)
        {
            if(cfInserted == NONE)
                clear_event_overflows();
            else if(uiState != EVENT_LOST && uiState != IDLE && uiState != DELAY
                    && uiState != INIT_SCREEN)
            {
                //K1 is the only live key on this screen — say so (the image
                //is untrusted, so SAVE is deliberately unreachable).
                uiext_wait_screen2("Event lost", "K1 ejects");
                uiState = EVENT_LOST;
            }
        }

        led_task();

        //While the QL is actively using the drive the UI is frozen: buttons
        //are ignored — including eject, which used to fire here instantly
        //with no dirty confirm (one press = data loss on a dirty cartridge).
        //Ejecting resumes in CARTRIDGE_READY once the QL deselects.
        if(!mdInUse)
            process_user_interface();

    }
}
