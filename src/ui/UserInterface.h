
#ifndef __USERINTERFACE__
#define __USERINTERFACE__

#define PIN_LED_ACTIVITY 10

#define PIN_BTN_BACK 12
#define PIN_BTN_NEXT 13
#define PIN_BTN_SELECT 14
#define PIN_BTN_SYSTEM 9

#define PIN_UI_DETECT 15

#define BUTTON_PRESSED(BUTTON) (!gpio_get(BUTTON))

#define CART_MDV_SIZE 174930
#define CART_MPD_SIZE 160140

#define MDV_PREAMBLE_SIZE 12
#define MDV_HEADER_SIZE 16
#define MDV_DATA_SIZE 646
#define MDV_PAD_SIZE 34
#define MDV_SECTOR_SIZE 686

#define MPD_HEADER_SIZE 16
#define MPD_DATA_SIZE 612

#define CARTRIDGE_HEADER_SIZE 16
#define CARTRIDGE_DATA_SIZE 612
#define CARTRIDGE_SECTOR_SIZE 628
#define CARTRIDGE_SECTOR_COUNT 255

#define PATH_BUFFER_SIZE 300

typedef enum
{
    IDLE,
    DELAY,
    INIT_SCREEN,
    WELCOME,
    SHOW_WAITING_SD_CARD,
    WAITING_SD_CARD,
    OPEN_FOLDER,
    BROWSE_FOLDER,
    SHOW_NO_FILES,
    NO_FILES,
    FILE_SELECTED,
    FILE_LOAD,
    CARTRIDGE_READY,
    EVENT_LOST

} USER_INTERFACE_STATE;

typedef enum
{
    NONE,
    MDV,
    MPD
} CARTRIDGE_FORMAT;

//Cartridge / drive state (owned by UserInterface.c). Exposed for the radio
//gate: BLE/OTA menu modes are only reachable with the cartridge ejected and
//the QL not using the drive (hard rule, PORT_PLAN).
extern CARTRIDGE_FORMAT cfInserted;
extern bool mdInUse;

typedef struct __attribute__((__packed__)) SECTOR_HEADER
{
    uint8_t HeaderData[14];
    uint16_t Checksum;

} SECTOR_HEADER_t;

typedef struct __attribute__((__packed__)) SECTOR_RECORD
{
    uint8_t HeaderData[2];
    uint16_t HeaderChecksum;
    uint8_t FilePreamble[8];
    uint8_t Data[512];
    uint16_t DataChecksum;
    uint8_t ExtraBytes[84];
    uint16_t ExtraBytesChecksum;

} SECTOR_RECORD_t;

typedef struct __attribute__((__packed__)) SECTOR
{
    SECTOR_HEADER_t Header;
    SECTOR_RECORD_t Record;

} SECTOR_t;

void RunUserInterface();
void debounce_button(uint button);

//Persist the System Tools display settings to the settings flash sector.
//No-op with a cartridge mounted or the drive in use (flash writes stall XIP).
void config_save_settings(void);

#endif