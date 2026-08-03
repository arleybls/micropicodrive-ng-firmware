// Band-menu / status-screen engine for the ST7735 UI, vendored from the UIExt
// sandbox (c:\VSCode Projects\UserInterfaceExtension_SPI, commit 057b1ae) and
// adapted to the MicroPicoDrive board: SPI1 display without RST/BL nets,
// K1-K4 buttons, vibro disabled until J3 is electrically verified.
#ifndef USERINTERFACEEXTENSION_H
#define USERINTERFACEEXTENSION_H

#include "ST7735_TFT.h"

// 1 on builds with the BLE OTA stack compiled in (arrives with the P4 phase).
#ifndef UIEXT_OTA_ENABLED
#define UIEXT_OTA_ENABLED 0
#endif
// Firmware version (CMake-injected; fallbacks for editors/analyzers)
#ifndef UIEXT_FW_VERSION_MAJOR
#define UIEXT_FW_VERSION_MAJOR 0
#endif
#ifndef UIEXT_FW_VERSION_MINOR
#define UIEXT_FW_VERSION_MINOR 0
#endif
#ifndef UIEXT_FW_VERSION_PATCH
#define UIEXT_FW_VERSION_PATCH 0
#endif

// ── Display geometry (landscape 160 × 80) ────────────────────────────────────
#define UIEXT_DISPLAY_WIDTH   160
#define UIEXT_DISPLAY_HEIGHT   80

// ── SPI display GPIO ──────────────────────────────────────────────────────────
// SCK → GP26 (SPI1), MOSI → GP27 (SPI1 TX). CS/DC come from CMake:
//   SPI_TFT_CS=20  SPI_TFT_DC=21  SPI_TFT_PORT=spi1
// No RST net (RC auto-reset + SWRESET; TFT_ENABLE_RESET stays undefined) and
// no backlight GPIO (hard-wired to 3V3).
#define UIEXT_TFT_SCK   26
#define UIEXT_TFT_MOSI  27

// ── Button GPIO assignments (K1-K4, active-low, internal pull-ups) ───────────
#define UIEXT_BTN_UP      12  // K1
#define UIEXT_BTN_DOWN    13  // K2
#define UIEXT_BTN_SELECT  14  // K3 (long press = CONFIG.CFG star-tag)
#define UIEXT_BTN_CONFIG   9  // K4 — opens System Tools

// ── Vibration motor ───────────────────────────────────────────────────────────
// J3/GP11 module type NOT yet electrically confirmed — keep disabled until it
// is (see docs/PORT_PLAN.md open items). 1 = sandbox soft-start PWM driver active.
#define UIEXT_VIBRO_ENABLED 0
#define UIEXT_VIBRO_PIN  11
#define UIEXT_VIBRO_MS    80
#define UIEXT_VIBRO_DUTY 255   // steady level after soft-start ramp; 255 = DC, no
                               // continuous switching (safe without flyback diode)

// ── Long-press continuous scroll ─────────────────────────────────────────────
#define UIEXT_LONG_PRESS_MS       1000
#define UIEXT_LONG_PRESS_REPEAT_MS  80

// ── Menu layout (Roboto_Thin_8, size 1) ──────────────────────────────────────
// Font metrics (16 px render size): ascent=15  descent=4  yAdvance=19
// Tallest glyph ('$') yOffset=-14 → row top: baseline-14 = 1 px from rect top.
// Deepest glyph ('(') reaches +3 below baseline → 18 px from rect top.
// 4 rows × 20 px = 80 px — exact fit with 2 px padding per row.
#define UIEXT_MENU_TEXT_X      2
// List rows (file browser): a red gutter bar marks the CONFIG.CFG-tagged
// item. All list text indents past bar + gap so tagged and untagged rows
// keep the same x (System Tools / title bands keep UIEXT_MENU_TEXT_X).
#define UIEXT_TAG_BAR_W        4
#define UIEXT_TAG_BAR_TOP      2
#define UIEXT_TAG_BAR_H       16
#define UIEXT_MENU_LIST_TEXT_X (UIEXT_TAG_BAR_W + 3)
#define UIEXT_MENU_ROW_TOP     0
#define UIEXT_MENU_ROW_PITCH  20
#define UIEXT_MENU_TEXT_SIZE   1   // GFX proportional font: size 1

// Offset within each row rect from the rect-top to the text baseline.
#define UIEXT_MENU_BASELINE_OFFSET  15

// Height of the rect reserved per row (= pitch for seamless background fill).
#define UIEXT_MENU_CHAR_HEIGHT  20

// Number of rows visible on-screen at once.
#define UIEXT_MENU_VISIBLE_ROWS  4

// Bits per pixel in the font bitmap (2 = 4 coverage levels for anti-aliasing).
#define UIEXT_FONT_BPP  2

// Buffer size for menu-item label strings.
#define UIEXT_MENU_ITEM_MAX_CHARS  32

// Typical xAdvance used for rough pixel-width estimates.
#define UIEXT_SCROLL_CHAR_PX  9

// ── Colors (RGB565) — runtime theme ──────────────────────────────────────────
typedef struct {
    uint16_t bg, text, sel_bg, sel_text;                  // MDV browser
    uint16_t cfg_bg, cfg_text, cfg_sel_bg, cfg_sel_text;  // System Tools + mode screens
    uint16_t title_bg, title_text, border;
} uiext_theme_t;

extern const uiext_theme_t *g_theme;   // active theme (Dark at boot)
void uiext_theme_set_dark(bool dark);
bool uiext_theme_is_dark(void);

#define UIEXT_COLOR_BG             (g_theme->bg)
#define UIEXT_COLOR_TEXT           (g_theme->text)
#define UIEXT_COLOR_SEL_BG         (g_theme->sel_bg)
#define UIEXT_COLOR_SEL_TEXT       (g_theme->sel_text)
#define UIEXT_COLOR_CFG_BG         (g_theme->cfg_bg)
#define UIEXT_COLOR_CFG_TEXT       (g_theme->cfg_text)
#define UIEXT_COLOR_CFG_SEL_BG     (g_theme->cfg_sel_bg)
#define UIEXT_COLOR_CFG_SEL_TEXT   (g_theme->cfg_sel_text)
#define UIEXT_COLOR_CFG_TITLE_BG   (g_theme->title_bg)
#define UIEXT_COLOR_CFG_TITLE_TEXT (g_theme->title_text)
#define UIEXT_COLOR_CFG_BORDER     (g_theme->border)
#define UIEXT_COLOR_PAIR_DOT       ST7735_BLUE  // pairing indicator — same in both themes
#define UIEXT_CFG_TEXT_PAD 6               // item text inset from the frame

// ── Horizontal idle-scroll animation ─────────────────────────────────────────
#define UIEXT_SCROLL_ANIM_ENABLED    1
#define UIEXT_SCROLL_ANIM_IDLE_MS    2000
#define UIEXT_SCROLL_ANIM_STEP_MS    60
#define UIEXT_SCROLL_MAX_TEXT_CHARS  42

// ── Confirm dialogs ──────────────────────────────────────────────────────────
// One timeout for every Yes/No confirm (all default to No/keep on expiry).
// The dialogs used to differ invisibly (15/30/60 s) for no reason.
#define UIEXT_CONFIRM_TIMEOUT_MS 30000

// ── Select blink animation ────────────────────────────────────────────────────
#define UIEXT_SELECT_BLINK_COUNT  2
#define UIEXT_SELECT_BLINK_MS     80

// ── Vertical navigation-scroll animation ─────────────────────────────────────
// Set per board in CMakeLists (study trap T8): the slide's vscroll_scratch[]
// is 160 x (80+20) x 2 = 32,000 bytes of bss, which does not fit the RP2040's
// budget — its RAM gap doubles as the FatFs heap. Deliberately NOT keyed to
// UIEXT_OTA_ENABLED: this is a RAM question, not a radio question, and
// conflating the two is what produced traps T1/T2/T7. Default 0 so a missing
// definition costs an animation, never a RAM overflow.
#ifndef UIEXT_VSCROLL_ANIM_ENABLED
#define UIEXT_VSCROLL_ANIM_ENABLED   0
#endif
#define UIEXT_VSCROLL_STEP_MS        0   // SPI transfer time provides pacing
#define UIEXT_VSCROLL_SCRATCH_HEIGHT (UIEXT_DISPLAY_HEIGHT + UIEXT_MENU_ROW_PITCH)

// ── Caption position (image preview screen) ───────────────────────────────────
typedef enum { CAPTION_TOP, CAPTION_MIDDLE, CAPTION_BOTTOM, CAPTION_POS_COUNT } caption_pos_t;
extern caption_pos_t g_caption_pos;

// Show the fixed red path title bar in the file browser (System Tools toggle).
extern bool g_path_title_enabled;

// Baseline offset from screen bottom for CAPTION_BOTTOM.
#define UIEXT_CAPTION_BOTTOM_BASELINE_OFFSET  4

// ── Sentinels returned by uiext_menu_run ─────────────────────────────────────
#define UIEXT_LONG_SELECT  (-2)
// A BLE Connect session changed the SD (push/delete/rename/setlabel) — the
// caller's directory listing is stale and must be rebuilt.
#define UIEXT_MENU_SD_CHANGED  (-3)
// The SD card was pulled while the listing was on screen — the caller must
// leave the browser (the list describes a card that is no longer there).
#define UIEXT_MENU_SD_GONE (-4)
#define UIEXT_SD_POLL_MS   1000   // card-presence poll cadence in the browser

// ── Public entry points ──────────────────────────────────────────────────────
#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"   // uint — includers must not need pico/stdlib.h first

// One-time hardware bring-up (display / buttons).
void setup_tft(void);
void setup_buttons(void);
void debounce_button(uint button);

// Band-menu over items (items[0] = unused header slot, selection starts at
// items[1]). Blocks until: SELECT returns the 1-based item index (after the
// select blink), long-SELECT returns UIEXT_LONG_SELECT (star-tag preview has
// already been toggled on the highlighted label). K4 runs System Tools
// internally (long K4 = BLE Connect shortcut) and the loop continues, unless
// a Connect session changed the SD — then UIEXT_MENU_SD_CHANGED is returned
// so the caller rebuilds its listing. *offset persists the scroll position.
int uiext_menu_run(char **items, int count, int *offset);

// Redraw the band menu (e.g. after the caller edits a label).
void uiext_menu_draw(char **items, int count, int offset);

// Pixel-clip a plain caption into dst (UIEXT_MENU_ITEM_MAX_CHARS + 3 bytes):
// copied when it fits max_px, otherwise clipped with a ".." suffix.
void uiext_pixel_clip(char *dst, const char *src, int max_px);

// Menu-label formatter (browser rows): [dir] / <dir> / NAME..EXT pixel clips.
// dst must be at least UIEXT_MENU_ITEM_MAX_CHARS + 3 bytes.
void format_menu_item(const char *src, char *dst);

// System Tools menu (also reachable from the waiting screen).
void uiext_system_tools(void);

// Rainbow corner position for settings persistence (0..4 = TL,TR,BL,BR,Off).
int  uiext_rainbow_get(void);
void uiext_rainbow_set(int pos);   // out-of-range values are ignored

// Full-screen cartridge/box-art screen: "<name>.thumb" sidecar when present
// + centered caption; without box art, "Mounted" over the name, centered
// mid-screen. Does not wait for input.
void uiext_cart_screen(const char *item_name, const char *caption);

// ── Status/confirm screen helpers (band-renderer based) ──────────────────────
void uiext_ota_screen(const char *title, const char *l1, const char *l2, const char *l3);

// Scrollable report screen over `lines` (UP/DOWN scrolls, SELECT/K4 exits).
void uiext_report_view(const char *title, const char **lines, int nlines);

// Yes/No confirm on the centered transfer-screen layout (default No).
bool uiext_ota_confirm_centered(const char *title, const char *line, uint32_t timeout_ms);

// Two-line Yes/No confirm (band grid, default No) that reports an explicit
// No separately from a timeout.
typedef enum {
    UIEXT_CONFIRM_NO = 0,
    UIEXT_CONFIRM_YES = 1,
    UIEXT_CONFIRM_TIMEOUT = 2,
} uiext_confirm_res_t;
uiext_confirm_res_t uiext_ota_confirm2(const char *title, const char *l1,
                                       const char *l2, uint32_t timeout_ms);
// Tri-state centered confirm: one body line, Yes/No directly below (no gap).
uiext_confirm_res_t uiext_ota_confirm_centered2(const char *title, const char *line,
                                                uint32_t timeout_ms);

// Selectable band menu over `items` — SELECT returns the index, K4 returns -1.
int uiext_menu_pick(const char *title, const char **items, int n);

// Centered screens: title bar; status shows one centered body line; progress
// draws "NN%" + centered counter (0-100).
void uiext_ota_title(const char *title);
void uiext_ota_status(const char *title, const char *body);
void uiext_ota_status2(const char *title, const char *l1, const char *l2);  // two centered lines
void uiext_ota_wait(const char *title, const char *name);  // waiting screen chrome
void uiext_wait_screen(const char *caption);               // same, without the title bar
void uiext_wait_screen2(const char *caption, const char *line2); // + second centered line
// Waiting-line animation (cli-spinners "dots12", replaces the old ellipsis):
// call with a free-running frame counter at UIEXT_WAIT_ANIM_MS cadence.
#define UIEXT_WAIT_ANIM_FRAMES 56
#define UIEXT_WAIT_ANIM_MS     80
void uiext_wait_anim_screen(const char *caption); // caption+anim pair, block-centered
void uiext_wait_anim(int frame);       // animation at the pair position
void uiext_ota_wait_anim(int frame);   // animation below a title band / hint layout
void uiext_boot_spinner(int frame);    // braille-cell spinner frame, classic spot
void uiext_ota_progress(const char *verb, const char *name, int pct); // full draw (on entry)
void uiext_ota_progress_update(int pct);                             // counter only
// 1.5x centered line; y_top = region top (UIEXT_BIG_CENTER_Y = body-centered).
void uiext_ota_big_text(const char *text, int y_top);
#define UIEXT_BIG_CENTER_Y 34
#if UIEXT_OTA_ENABLED
bool uiext_ota_confirm(const char *title, const char *l1, const char *l2, uint32_t timeout_ms);
#endif

#endif // USERINTERFACEEXTENSION_H
