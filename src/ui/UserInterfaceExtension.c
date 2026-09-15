// Band-menu / status-screen engine, vendored from the UIExt sandbox
// (c:\VSCode Projects\UserInterfaceExtension_SPI\UserInterfaceExtension.c,
// commit 057b1ae). Adaptations for MicroPicoDrive:
//   - no RST net: TFT_ENABLE_RESET undefined, >=50 ms RC-reset settle before
//     init, SWRESET (inside Rcmd1) does the reset
//   - no backlight GPIO (hard-wired 3V3)
//   - main() and the standalone sd_menu browse loop removed; UserInterface.c
//     owns the flow and calls uiext_menu_run/uiext_cart_screen instead
//   - long-press SELECT = CONFIG.CFG star-tag preview (kept from the previous
//     MicroPicoDrive menu); K4 opens System Tools
//   - vibro enabled: J3/GP11 wiring confirmed against the KiCad project
//     (driver-equipped breakout, motor rail 5V — see UserInterfaceExtension.h)
#include <stddef.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "ST7735_TFT.h"
#include "Roboto_Thin_8.h"
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include "UserInterfaceExtension.h"
#include "sd_check.h"
#include "sd_menu.h"
#include "sd_thumb.h"
#include "sys_info.h"
// Both boards need this: cfInserted/mdInUse (radio-mode gate on 2 W, BOOTSEL
// eject gate on Lite), PIN_LED_ACTIVITY for the LED Test entry, and
// config_save_settings(). Gating it broke the RP2040 build — study trap T3.
#include "UserInterface.h"
#if UIEXT_OTA_ENABLED
#include "ota_ble.h"
#else
#include "pico/bootrom.h"    // reset_usb_boot — Lite's System Tools BOOTSEL entry
#endif
#include "hardware/pwm.h"
#include "hardware/spi.h"

// ── Globals ───────────────────────────────────────────────────────────────────
caption_pos_t g_caption_pos = CAPTION_BOTTOM;
bool g_path_title_enabled = false;   // path title bar off by default

// ── Themes ───────────────────────────────────────────────────────────────────
static const uiext_theme_t THEME_DARK = {
    .bg = ST7735_BLACK,  .text = ST7735_WHITE,
    .sel_bg = ST7735_WHITE, .sel_text = ST7735_BLACK,   // browser selection = white bar
    .cfg_bg = ST7735_BLACK, .cfg_text = ST7735_WHITE,
    .cfg_sel_bg = ST7735_WHITE, .cfg_sel_text = ST7735_BLACK,
    .title_bg = ST7735_RED, .title_text = ST7735_WHITE,
    .border = ST7735_BLACK,
};
static const uiext_theme_t THEME_LIGHT = {
    .bg = ST7735_WHITE,  .text = ST7735_BLACK,
    .sel_bg = ST7735_BLACK, .sel_text = ST7735_WHITE,   // inverse: white bar would vanish here
    .cfg_bg = ST7735_WHITE, .cfg_text = ST7735_BLACK,
    .cfg_sel_bg = ST7735_BLACK, .cfg_sel_text = ST7735_WHITE,
    .title_bg = ST7735_RED, .title_text = ST7735_WHITE, // title bar: same both modes
    .border = ST7735_WHITE,
};

const uiext_theme_t *g_theme = &THEME_DARK;
static bool s_theme_dark = true;

void uiext_theme_set_dark(bool dark) {
    s_theme_dark = dark;
    g_theme = dark ? &THEME_DARK : &THEME_LIGHT;
}

bool uiext_theme_is_dark(void) { return s_theme_dark; }

// ── Sinclair rainbow strip ───────────────────────────────────────────────────
typedef enum { RB_TOP_LEFT, RB_TOP_RIGHT, RB_BTM_LEFT, RB_BTM_RIGHT, RB_OFF } rainbow_pos_t;
static rainbow_pos_t s_rainbow_pos = RB_TOP_RIGHT;
static const char *const rainbow_labels[] =
    { "Top Left", "Top Right", "Btm Left", "Btm Right", "Off" };

int uiext_rainbow_get(void) { return (int)s_rainbow_pos; }

void uiext_rainbow_set(int pos) {
    if (pos >= 0 && pos <= (int)RB_OFF)
        s_rainbow_pos = (rainbow_pos_t)pos;
}

#define RB_STRIPE_W 10
#define RB_W        (4 * RB_STRIPE_W)
#define RB_H        4
#define RB_MARGIN   4
static const uint16_t RB_COLS[4] = { 0xD800, 0xF500, 0x0600, 0x03DB };

// Top-left origin of the strip (x excludes the slant, which adds up to RB_H-1).
static bool rainbow_geom(int *x0, int *y0) {
    switch (s_rainbow_pos) {
        case RB_TOP_LEFT:  *x0 = RB_MARGIN; *y0 = 0; return true;
        case RB_TOP_RIGHT: *x0 = UIEXT_DISPLAY_WIDTH - RB_MARGIN - RB_W - (RB_H - 1); *y0 = 0; return true;
        case RB_BTM_LEFT:  *x0 = RB_MARGIN; *y0 = UIEXT_DISPLAY_HEIGHT - RB_H; return true;
        case RB_BTM_RIGHT: *x0 = UIEXT_DISPLAY_WIDTH - RB_MARGIN - RB_W - (RB_H - 1);
                           *y0 = UIEXT_DISPLAY_HEIGHT - RB_H; return true;
        default: return false;
    }
}

// Composite the strip into a full-width RGB565 buffer that will be blitted at
// screen row `buf_y0` with `buf_h` rows — same SPI burst, so no flicker.
static void rainbow_stamp(uint8_t *buf, int buf_y0, int buf_h) {
    int x0, y0;
    if (!rainbow_geom(&x0, &y0)) return;
    for (int r = 0; r < RB_H; r++) {
        int sy = y0 + r;
        if (sy < buf_y0 || sy >= buf_y0 + buf_h) continue;
        int row = sy - buf_y0;
        int x = x0 + (RB_H - 1 - r);          // 1px/row slant, leaning "/"
        for (int s = 0; s < 4; s++) {
            uint8_t hi = (uint8_t)(RB_COLS[s] >> 8), lo = (uint8_t)(RB_COLS[s] & 0xFF);
            for (int i = 0; i < RB_STRIPE_W; i++) {
                int px = x + s * RB_STRIPE_W + i;
                if (px < 0 || px >= UIEXT_DISPLAY_WIDTH) continue;
                int idx = (row * UIEXT_DISPLAY_WIDTH + px) * 2;
                buf[idx]     = hi;
                buf[idx + 1] = lo;
            }
        }
    }
}

// Direct draw for one-shot screens whose body is cleared with fillRectangle.
static void uiext_draw_rainbow(void) {
    int x0, y0;
    if (!rainbow_geom(&x0, &y0)) return;
    for (int r = 0; r < RB_H; r++) {
        int x = x0 + (RB_H - 1 - r);
        for (int s = 0; s < 4; s++)
            fillRectangle((uint8_t)(x + s * RB_STRIPE_W), (uint8_t)(y0 + r),
                          RB_STRIPE_W, 1, RB_COLS[s]);
    }
}

// Library-internal state — non-static in ST7735_TFT.c, accessible via extern.
extern uint8_t _colstart, _rowstart, _tft_type;

// Shared row render buffer — UIEXT_DISPLAY_WIDTH × UIEXT_MENU_CHAR_HEIGHT × 2 bytes.
static uint8_t menu_row_buf[UIEXT_DISPLAY_WIDTH * UIEXT_MENU_CHAR_HEIGHT * 2];

// ── Custom display init for 80 × 160 ST7735S ─────────────────────────────────
// The library's built-in variants target 128 × 160. This variant sets the
// correct CASET/RASET window (0–79 cols, 0–159 rows) and stores the 26-pixel
// column offset so setAddrWindow maps logical (0,0) → hardware (26,1).
static void TFT_80x160_Initialize(void) {
#if defined TFT_ENABLE_RESET
    TFT_ResetPIN();
#endif
    tft_dc_low();
    Rcmd1();
    write_command(ST7735_CASET);
    write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x4F);  // 0..79 columns
    write_command(ST7735_RASET);
    write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x9F);  // 0..159 rows
    Rcmd3();
    // This 0.96" 80x160 ST7735S variant needs inverted mode — without it the
    // panel complements every pixel (red showed as yellow, white as black).
    write_command(ST7735_INVON);
    write_command(ST7735_MADCTL);
    write_data(0xC0);                    // black-tab orientation
    _colstart = 26;
    _rowstart  = 1;
    _tft_type  = 1;
}

void setup_tft(void) {
    // No RST net: the panel's RC auto-reset needs time after power-on before
    // it accepts commands; SWRESET inside Rcmd1() then does the real reset.
    sleep_ms(50);
    spi_init(SPI_TFT_PORT, 40 * 1000 * 1000);
    gpio_set_function(UIEXT_TFT_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(UIEXT_TFT_MOSI, GPIO_FUNC_SPI);
    tft_spi_init();              // configures CS / DC as GPIO outputs
    TFT_80x160_Initialize();
    setRotation(3);              // landscape: _width=160, _height=128
    // setRotation writes MADCTL last with the lib's RGB order, but this panel
    // is wired BGR — re-issue the rotation-3 bits with BGR set, AFTER it.
    write_command(ST7735_MADCTL);
    write_data(0x68);            // MX | MV | BGR
    tft_height = 80;             // override to actual panel height
    setFont(&Roboto_Thin_8);
    setTextWrap(false);
    fillScreen(UIEXT_COLOR_BG);
}

// ── Font metrics helper ───────────────────────────────────────────────────────
static uint16_t label_pixel_width(const char *text) {
    uint16_t w = 0;
    for (const char *p = text; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c >= _gfxFont->first && c <= _gfxFont->last)
            w += _gfxFont->glyph[c - _gfxFont->first].xAdvance;
    }
    return w;
}

// Copy src into dst (UIEXT_MENU_ITEM_MAX_CHARS + 3 bytes), pixel-clipping to
// max_px with a ".." suffix when too wide. For plain captions — no <dir> /
// [dir] / .ext decoration (that's format_menu_item's job).
void uiext_pixel_clip(char *dst, const char *src, int max_px) {
    if ((int)label_pixel_width(src) <= max_px) {
        strncpy(dst, src, UIEXT_MENU_ITEM_MAX_CHARS + 2);
        dst[UIEXT_MENU_ITEM_MAX_CHARS + 2] = '\0';
        return;
    }
    int avail = max_px - (int)label_pixel_width("..");
    int pos = 0;
    uint16_t used = 0;
    for (const char *p = src; *p && pos < UIEXT_MENU_ITEM_MAX_CHARS; p++) {
        uint8_t c = (uint8_t)*p;
        uint16_t cw = (c >= _gfxFont->first && c <= _gfxFont->last)
                      ? _gfxFont->glyph[c - _gfxFont->first].xAdvance : 0;
        if ((int)(used + cw) > avail) break;
        dst[pos++] = (char)c;
        used += cw;
    }
    dst[pos] = '\0';
    strcat(dst, "..");
}

// ── Menu label formatter ──────────────────────────────────────────────────────
// Clips by pixel width (proportional font), not character count.
//   [bracket]  → pixel-clipped to "[clip..]" if too wide
//   no-dot     → wrapped in < >, pixel-clipped to "<clip..>" if too wide
//   .ext, fits → copied unchanged
//   .ext, long → base pixel-clipped, ".." + ext appended
// dst must be at least UIEXT_MENU_ITEM_MAX_CHARS + 3 bytes.
void format_menu_item(const char *src, char *dst) {
    int max_px = UIEXT_DISPLAY_WIDTH - UIEXT_MENU_LIST_TEXT_X;

    if (src[0] == '[') {
        // Bracketed [dir] label: same pixel budget as files, clipped to
        // "[name..]" so the closing bracket (the directory cue) survives.
        if ((int)label_pixel_width(src) <= max_px) {
            strncpy(dst, src, UIEXT_MENU_ITEM_MAX_CHARS + 2);
            dst[UIEXT_MENU_ITEM_MAX_CHARS + 2] = '\0';
            return;
        }
        static int s_px_lbr = -1, s_px_dotdotrbr = -1;
        if (s_px_lbr < 0) {
            s_px_lbr       = (int)label_pixel_width("[");
            s_px_dotdotrbr = (int)label_pixel_width("..]");
        }
        int avail = max_px - s_px_lbr - s_px_dotdotrbr;
        dst[0] = '[';
        int pos = 1;
        uint16_t used = 0;
        for (const char *p = src + 1; *p && *p != ']' &&
                                      pos < UIEXT_MENU_ITEM_MAX_CHARS - 1; p++) {
            uint8_t c = (uint8_t)*p;
            uint16_t cw = (c >= _gfxFont->first && c <= _gfxFont->last)
                          ? _gfxFont->glyph[c - _gfxFont->first].xAdvance : 0;
            if ((int)(used + cw) > avail) break;
            dst[pos++] = (char)c;
            used += cw;
        }
        dst[pos] = '\0';
        strcat(dst, "..]");
        return;
    }

    // Cache the pixel widths of the fixed decoration strings — font-constant values.
    static int s_px_lt = -1, s_px_dotdotgt = -1;
    if (s_px_lt < 0) {
        s_px_lt       = (int)label_pixel_width("<");
        s_px_dotdotgt = (int)label_pixel_width("..>");
    }

    if (strchr(src, '.') == NULL) {
        // Directory: wrap in < >
        int slen = (int)strlen(src);
        dst[0] = '<';
        memcpy(dst + 1, src, slen);
        dst[1 + slen] = '>';
        dst[2 + slen] = '\0';
        int full_w = (int)label_pixel_width(dst);
        if (full_w <= max_px) return;

        // Doesn't fit: clip name so "<name..>" fits
        int avail = max_px - s_px_lt - s_px_dotdotgt;
        int pos = 1;
        uint16_t used = 0;
        for (const char *p = src; *p; p++) {
            uint8_t c = (uint8_t)*p;
            uint16_t cw = (c >= _gfxFont->first && c <= _gfxFont->last)
                          ? _gfxFont->glyph[c - _gfxFont->first].xAdvance : 0;
            if ((int)(used + cw) > avail) break;
            dst[pos++] = (char)c;
            used += cw;
        }
        dst[pos] = '\0';
        strcat(dst, "..>");
        return;
    }

    // File with extension
    if ((int)label_pixel_width(src) <= max_px) {
        strncpy(dst, src, UIEXT_MENU_ITEM_MAX_CHARS + 2);
        dst[UIEXT_MENU_ITEM_MAX_CHARS + 2] = '\0';
        return;
    }

    // Clip: <base>..<ext>  (ext = chars after last dot, no extra dot in suffix)
    const char *last_dot = strrchr(src, '.');
    const char *ext      = last_dot + 1;
    char suffix[UIEXT_MENU_ITEM_MAX_CHARS + 3];
    snprintf(suffix, sizeof(suffix), "..%s", ext);
    int avail = max_px - (int)label_pixel_width(suffix);

    int pos = 0;
    uint16_t used = 0;
    for (const char *p = src; p < last_dot; p++) {
        uint8_t c = (uint8_t)*p;
        uint16_t cw = (c >= _gfxFont->first && c <= _gfxFont->last)
                      ? _gfxFont->glyph[c - _gfxFont->first].xAdvance : 0;
        if ((int)(used + cw) > avail) break;
        dst[pos++] = (char)c;
        used += cw;
    }
    dst[pos] = '\0';
    strcat(dst, suffix);
}

// ── Anti-aliased glyph rasterizer ─────────────────────────────────────────────
// Blend two RGB565 colours using a 2-bit coverage level (0=bg .. 3=fg).
static inline uint16_t blend_rgb565(uint16_t fg, uint16_t bg, uint8_t level) {
    if (level == 0) return bg;
    if (level == 3) return fg;
    uint8_t r_fg = (fg >> 11) & 0x1F,  r_bg = (bg >> 11) & 0x1F;
    uint8_t g_fg = (fg >>  5) & 0x3F,  g_bg = (bg >>  5) & 0x3F;
    uint8_t b_fg =  fg        & 0x1F,  b_bg =  bg        & 0x1F;
    uint8_t r, g, b;
    if (level == 1) {
        r = (uint8_t)(((uint16_t)r_fg + r_bg * 2u) * 43u >> 7);
        g = (uint8_t)(((uint16_t)g_fg + g_bg * 2u) * 43u >> 7);
        b = (uint8_t)(((uint16_t)b_fg + b_bg * 2u) * 43u >> 7);
    } else {
        r = (uint8_t)(((uint16_t)r_fg * 2u + r_bg) * 43u >> 7);
        g = (uint8_t)(((uint16_t)g_fg * 2u + g_bg) * 43u >> 7);
        b = (uint8_t)(((uint16_t)b_fg * 2u + b_bg) * 43u >> 7);
    }
    return ((uint16_t)r << 11) | ((uint16_t)g << 5) | b;
}

// Rasterise `label` glyphs into `buf` (UIEXT_DISPLAY_WIDTH × UIEXT_MENU_CHAR_HEIGHT,
// RGB565 row-major) with 2-bit AA.  `cursor_x` is the initial pen position (may be
// negative for hscroll).
static void rasterize_row(uint8_t *buf, const char *label,
                           uint16_t fg, uint16_t bg, int cursor_x) {
    uint8_t bg_hi = (uint8_t)(bg >> 8), bg_lo = (uint8_t)(bg & 0xFF);
    for (int i = 0; i < UIEXT_DISPLAY_WIDTH * UIEXT_MENU_CHAR_HEIGHT * 2; i += 2) {
        buf[i]     = bg_hi;
        buf[i + 1] = bg_lo;
    }
    if (!label || !label[0]) return;
    uint8_t *bm = _gfxFont->bitmap;
    for (const char *p = label; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < _gfxFont->first || c > _gfxFont->last) continue;
        GFXglyph *g = &_gfxFont->glyph[c - _gfxFont->first];
        if (cursor_x >= UIEXT_DISPLAY_WIDTH) break;
        if (cursor_x + (int)g->xOffset + (int)g->width > 0) {
            uint16_t bo    = g->bitmapOffset;
            uint8_t  bits  = 0;
            uint8_t  bpass = 0;  // pixels consumed from current byte (0-3)
            for (uint8_t yy = 0; yy < g->height; yy++) {
                for (uint8_t xx = 0; xx < g->width; xx++) {
                    if (!bpass) bits = bm[bo++];
                    uint8_t level = (bits >> 6) & 0x03;
                    bits <<= 2;
                    bpass = (bpass + 1) & 3;
                    if (level > 0) {
                        int px = cursor_x + (int)g->xOffset + (int)xx;
                        int py = (int)UIEXT_MENU_BASELINE_OFFSET + (int)g->yOffset + (int)yy;
                        if (px >= 0 && px < UIEXT_DISPLAY_WIDTH &&
                            py >= 0 && py < UIEXT_MENU_CHAR_HEIGHT) {
                            uint16_t col = blend_rgb565(fg, bg, level);
                            int idx = (py * UIEXT_DISPLAY_WIDTH + px) * 2;
                            buf[idx]     = (uint8_t)(col >> 8);
                            buf[idx + 1] = (uint8_t)(col & 0xFF);
                        }
                    }
                }
            }
        }
        cursor_x += (int)g->xAdvance;
    }
}

// ── CONFIG.CFG tag bar ────────────────────────────────────────────────────────
// Red bar in the left gutter marking the auto-load-tagged item. The '*'
// label prefix stays as the data marker but is stripped before rasterising;
// list text starts at UIEXT_MENU_LIST_TEXT_X (clear of the bar) on every
// row, so tagged and untagged items keep the same text x.
static void tag_bar_stamp(uint8_t *rowbuf) {
    for (int y = UIEXT_TAG_BAR_TOP; y < UIEXT_TAG_BAR_TOP + UIEXT_TAG_BAR_H; y++) {
        int idx = y * UIEXT_DISPLAY_WIDTH * 2;
        for (int x = 0; x < UIEXT_TAG_BAR_W; x++) {
            rowbuf[idx + x * 2]     = (uint8_t)(ST7735_RED >> 8);
            rowbuf[idx + x * 2 + 1] = (uint8_t)(ST7735_RED & 0xFF);
        }
    }
}

// ── Row drawing helper ────────────────────────────────────────────────────────
static void draw_menu_row(int row_idx, const char *label, bool tagged, bool selected,
                          uint16_t bg_normal, uint16_t fg_normal,
                          uint16_t bg_sel,    uint16_t fg_sel) {
    uint16_t bg = selected ? bg_sel    : bg_normal;
    uint16_t fg = selected ? fg_sel    : fg_normal;
    rasterize_row(menu_row_buf, label, fg, bg, UIEXT_MENU_LIST_TEXT_X);
    if (tagged) tag_bar_stamp(menu_row_buf);
    uint8_t y = (uint8_t)(UIEXT_MENU_ROW_TOP + row_idx * UIEXT_MENU_ROW_PITCH);
    rainbow_stamp(menu_row_buf, y, UIEXT_MENU_CHAR_HEIGHT);
    setAddrWindow(0, y,
                  (uint8_t)(UIEXT_DISPLAY_WIDTH  - 1),
                  (uint8_t)(y + UIEXT_MENU_CHAR_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
    tft_cs_high();
}

// ── Main menu draw ────────────────────────────────────────────────────────────
static void draw_cfg_band(int band_idx, const char *label,
                          uint16_t fg, uint16_t bg, int text_x);   // defined below

static void drawMenu(char **menu_items, int menu_size, int offset) {
    int first_row = 0;
    if (g_path_title_enabled) {
        // Fixed title: current path, left-aligned, trailing slash on subdirs.
        // Deep paths keep their tail by PIXEL budget ("..<tail>/" must fit
        // the band) — the old 33-char keep let wide paths overflow right and
        // lose the trailing "/" cue.
        char title[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        const char *p = sd_menu_cur_path();
        if (strcmp(p, "/") == 0) {
            snprintf(title, sizeof(title), "/");
        } else {
            int max_px = UIEXT_DISPLAY_WIDTH - (UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
            size_t l = strlen(p);
            int slash_px = (int)label_pixel_width("/");
            if (l <= sizeof(title) - 2 &&
                (int)label_pixel_width(p) + slash_px <= max_px) {
                snprintf(title, sizeof(title), "%s/", p);
            } else {
                int avail = max_px - (int)label_pixel_width("..") - slash_px;
                const char *tail = p + l;
                uint16_t used = 0;
                while (tail > p && (size_t)(l - (size_t)(tail - p)) < sizeof(title) - 4) {
                    uint8_t c = (uint8_t)tail[-1];
                    uint16_t cw = (c >= _gfxFont->first && c <= _gfxFont->last)
                                  ? _gfxFont->glyph[c - _gfxFont->first].xAdvance : 0;
                    if ((int)(used + cw) > avail) break;
                    used += cw;
                    tail--;
                }
                snprintf(title, sizeof(title), "..%s/", tail);
            }
        }
        draw_cfg_band(0, title, UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG,
                      UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
        first_row = 1;
    }

    // Title off: at the top of the list (offset 0) the first item sits on the
    // top line carrying the highlight; from the second item on, the highlight
    // stays on the second line with the context item above.
    int shift  = (!g_path_title_enabled && offset == 0) ? 1 : 0;
    int hl_row = (!g_path_title_enabled && offset == 0) ? 0 : 1;
    int items_left = menu_size - offset;
    for (int row = first_row; row < UIEXT_MENU_VISIBLE_ROWS; row++) {
        int idx = row + offset + shift;
        char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        label[0] = '\0';
        bool tagged = false;
        if (idx >= 1 && idx < menu_size) {
            const char *raw = menu_items[idx];
            tagged = (raw[0] == '*');
            format_menu_item(tagged ? raw + 1 : raw, label);
        }
        draw_menu_row(row, label, tagged, (row == hl_row && items_left > 1),
                      UIEXT_COLOR_BG,    UIEXT_COLOR_TEXT,
                      UIEXT_COLOR_SEL_BG, UIEXT_COLOR_SEL_TEXT);
    }
}

void uiext_menu_draw(char **items, int count, int offset) {
    drawMenu(items, count, offset);
}

// ── Vibration motor ───────────────────────────────────────────────────────────
#if UIEXT_VIBRO_ENABLED
// Returns the soft-start ramp duration in ms, already spent when it returns.
static uint32_t vibro_start(void) {
    uint slice   = pwm_gpio_to_slice_num(UIEXT_VIBRO_PIN);
    uint channel = pwm_gpio_to_channel(UIEXT_VIBRO_PIN);
    gpio_set_function(UIEXT_VIBRO_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv(slice, 25.0f);  // ~19.5 kHz — above audible, slow enough to drive a motor
    pwm_set_wrap(slice, 255);
    pwm_set_chan_level(slice, channel, 0);
    pwm_set_enabled(slice, true);
    // Soft-start: ramp the duty instead of a full-power kick — the motor's
    // inrush current sags the 3V3 rail and visibly dims the backlight.
    for (uint level = 16; level < UIEXT_VIBRO_DUTY; level += 16) {
        pwm_set_chan_level(slice, channel, level);
        sleep_ms(1);
    }
    pwm_set_chan_level(slice, channel, UIEXT_VIBRO_DUTY);
    return UIEXT_VIBRO_DUTY / 16;
}

static void vibro_stop(void) {
    pwm_set_enabled(pwm_gpio_to_slice_num(UIEXT_VIBRO_PIN), false);
    gpio_set_function(UIEXT_VIBRO_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(UIEXT_VIBRO_PIN, GPIO_OUT);
    gpio_put(UIEXT_VIBRO_PIN, 0);
}
#endif

// Set while a sustained load/save run holds the motor on; vibrate() then
// no-ops so an event buzz cannot stop the run half-way.
static bool s_vibro_running = false;

// Master switch: Off silences every haptic (events and load/save runs).
// The LED & Motor Test bypasses vibrate() on purpose, so it stays testable.
static bool s_vibro_master = true;

bool uiext_vibro_master_get(void) { return s_vibro_master; }
void uiext_vibro_master_set(bool on) { s_vibro_master = on; }

static void vibrate(uint32_t ms) {
#if !UIEXT_VIBRO_ENABLED
    (void)ms;
#else
    if (s_vibro_running || !s_vibro_master) return;
    uint32_t ramp_ms = vibro_start();
    sleep_ms(ms > ramp_ms ? ms - ramp_ms : 0);
    vibro_stop();
#endif
}

// Per-event haptics: enables persisted by config_save/load_settings (like the
// rainbow position); default on, so pre-feature saved records (0xFF fields)
// load as enabled without rolling SETTINGS_MAGIC.
static bool s_vibro_ev[UIEXT_VEV_COUNT] = { true, true, true, true };

// Load/Save sustained-run setting: index into the tail table; 0 disables the
// run. Persisted as the raw index — out-of-range (a pre-feature record's
// 0xFF) is ignored by the setter, keeping the 500 ms default.
static const uint16_t sdop_tail_ms[] = { 0, 250, 500, 1000 };
static const char *const sdop_labels[] = { "Off", "250ms", "500ms", "1s" };
#define SDOP_POS_COUNT 4
static int s_vibro_sdop_pos = 2;   // 500 ms

int uiext_vibro_sdop_get(void) { return s_vibro_sdop_pos; }

void uiext_vibro_sdop_set(int pos) {
    if (pos >= 0 && pos < SDOP_POS_COUNT) s_vibro_sdop_pos = pos;
}

bool uiext_vibro_ev_get(int ev) {
    return ev >= 0 && ev < UIEXT_VEV_COUNT && s_vibro_ev[ev];
}

void uiext_vibro_ev_set(int ev, bool on) {
    if (ev >= 0 && ev < UIEXT_VEV_COUNT) s_vibro_ev[ev] = on;
}

void uiext_vibrate_event(int ev) {
    if (!uiext_vibro_ev_get(ev)) return;
    vibrate(UIEXT_VIBRO_MS);
    if (ev == UIEXT_VEV_ALERT) {           // double buzz: bad news feels different
        sleep_ms(60);
        vibrate(UIEXT_VIBRO_MS);
    }
}

void uiext_vibro_run_begin(void) {
#if UIEXT_VIBRO_ENABLED
    if (s_vibro_running || !s_vibro_master || s_vibro_sdop_pos == 0) return;
    vibro_start();
    s_vibro_running = true;
#endif
}

void uiext_vibro_run_end(void) {
#if UIEXT_VIBRO_ENABLED
    if (!s_vibro_running) return;
    sleep_ms(sdop_tail_ms[s_vibro_sdop_pos]);   // spin-down tail (sandbox feel)
    vibro_stop();
    s_vibro_running = false;
#endif
}

// ── Button helpers ────────────────────────────────────────────────────────────
void setup_buttons(void) {
    gpio_init(UIEXT_BTN_UP);     gpio_set_dir(UIEXT_BTN_UP,     GPIO_IN); gpio_pull_up(UIEXT_BTN_UP);
    gpio_init(UIEXT_BTN_DOWN);   gpio_set_dir(UIEXT_BTN_DOWN,   GPIO_IN); gpio_pull_up(UIEXT_BTN_DOWN);
    gpio_init(UIEXT_BTN_SELECT); gpio_set_dir(UIEXT_BTN_SELECT, GPIO_IN); gpio_pull_up(UIEXT_BTN_SELECT);
    gpio_init(UIEXT_BTN_CONFIG); gpio_set_dir(UIEXT_BTN_CONFIG, GPIO_IN); gpio_pull_up(UIEXT_BTN_CONFIG);
}

void debounce_button(uint button) {
    while (gpio_get(button) == 0) sleep_ms(20);  // wait for release (active-low)
    sleep_ms(200);
}

// t_press: timestamp captured the moment the button was first detected.
static bool wait_press_type(uint btn, uint32_t t_press) {
    while (gpio_get(btn) == 0) {  // active-low: 0 = pressed
        if ((to_ms_since_boot(get_absolute_time()) - t_press) >= (uint32_t)UIEXT_LONG_PRESS_MS) {
            uiext_vibrate_event(UIEXT_VEV_LONGPRESS);   // release cue at the threshold
            return true;
        }
        sleep_ms(10);
    }
    sleep_ms(200);
    return false;
}

// ── Select blink ─────────────────────────────────────────────────────────────
static void do_select_blink(const char *raw_name, int band) {
    char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
    bool tagged = (raw_name[0] == '*');
    format_menu_item(tagged ? raw_name + 1 : raw_name, label);
    for (int i = 0; i < UIEXT_SELECT_BLINK_COUNT; i++) {
        draw_menu_row(band, label, tagged, false,
                      UIEXT_COLOR_BG, UIEXT_COLOR_TEXT, UIEXT_COLOR_SEL_BG, UIEXT_COLOR_SEL_TEXT);
        sleep_ms(UIEXT_SELECT_BLINK_MS);
        draw_menu_row(band, label, tagged, true,
                      UIEXT_COLOR_BG, UIEXT_COLOR_TEXT, UIEXT_COLOR_SEL_BG, UIEXT_COLOR_SEL_TEXT);
        sleep_ms(UIEXT_SELECT_BLINK_MS);
    }
}

// ── Cartridge / box-art screen ────────────────────────────────────────────────
// "<name>.thumb" sidecar (raw RGB565 or BMP) full-bleed when present, plus a
// centered caption. Without box art: "Mounted" over the cartridge name,
// centered as a block in the middle (the caption-position setting does not
// apply there). Non-blocking (the caller owns the flow; the sandbox's
// button-wait was removed).
void uiext_cart_screen(const char *item_name, const char *caption) {
    fillScreen(UIEXT_COLOR_BG);

    if (!sd_thumb_show(item_name)) {
        const char *lines[2] = { "Mounted", (caption && caption[0]) ? caption : "" };
        int top = (UIEXT_DISPLAY_HEIGHT - 2 * UIEXT_MENU_CHAR_HEIGHT) / 2;
        for (int i = 0; i < 2; i++) {
            int x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width(lines[i])) / 2;
            if (x < 0) x = 0;
            rasterize_row(menu_row_buf, lines[i], UIEXT_COLOR_TEXT, UIEXT_COLOR_BG, x);
            uint8_t y = (uint8_t)(top + i * UIEXT_MENU_CHAR_HEIGHT);
            setAddrWindow(0, y, (uint8_t)(UIEXT_DISPLAY_WIDTH - 1),
                          (uint8_t)(y + UIEXT_MENU_CHAR_HEIGHT - 1));
            tft_dc_high();
            tft_cs_low();
            spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
            tft_cs_high();
        }
        return;
    }

    if (!caption || !caption[0]) return;

    // Horizontally centred caption (baseline y depends on caption position)
    int text_w = (int)label_pixel_width(caption);
    int text_x = (UIEXT_DISPLAY_WIDTH - text_w) / 2;
    if (text_x < 0) text_x = 0;

    int text_y;  // baseline position
    switch (g_caption_pos) {
        case CAPTION_TOP:    text_y = UIEXT_MENU_BASELINE_OFFSET; break;
        case CAPTION_MIDDLE: text_y = UIEXT_DISPLAY_HEIGHT / 2 + 6; break;
        case CAPTION_BOTTOM: text_y = UIEXT_DISPLAY_HEIGHT - UIEXT_CAPTION_BOTTOM_BASELINE_OFFSET; break;
        default:             text_y = UIEXT_DISPLAY_HEIGHT / 2 + 6; break;
    }
    // AA rasterizer — library drawChar reads 1-bit bitmaps; ours are 2-bit.
    int row_top = text_y - (int)UIEXT_MENU_BASELINE_OFFSET;
    if (row_top < 0) row_top = 0;
    if (row_top + (int)UIEXT_MENU_CHAR_HEIGHT > (int)UIEXT_DISPLAY_HEIGHT)
        row_top = (int)UIEXT_DISPLAY_HEIGHT - (int)UIEXT_MENU_CHAR_HEIGHT;
    // Theme text color, not hardcoded white — white on the light theme's
    // white band made the caption invisible.
    rasterize_row(menu_row_buf, caption, UIEXT_COLOR_TEXT, UIEXT_COLOR_BG, text_x);
    setAddrWindow(0, (uint8_t)row_top,
                  (uint8_t)(UIEXT_DISPLAY_WIDTH - 1),
                  (uint8_t)(row_top + UIEXT_MENU_CHAR_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
    tft_cs_high();
}

// ── System Tools menu ─────────────────────────────────────────────────────────
// CFG_BOOTSEL is Lite-only (RP2040 has no A/B slots, so USB BOOTSEL is one of
// its two field-update routes); the five BLE entries are mainline-only. Both
// sets live here, gated — study trap T7: mainline had DELETED the BOOTSEL
// entry outright, so a fold would have removed it from the RP2040 menu with no
// error at all.
enum { CFG_CAPTION, CFG_THEME, CFG_PATHBAR, CFG_RAINBOW, CFG_CONNECT, CFG_OTA, CFG_PAIR, CFG_PAIRED, CFG_REVERT, CFG_SDCHECK, CFG_SYSINFO,
       CFG_LEDTEST, CFG_BOOTSEL, CFG_EXIT,
       CFG_SEP_UI,     // non-selectable "UI" group separator (dotted rules)
       CFG_SEP_MOTOR,  // ditto for the haptic-event toggles
       CFG_VIB_MASTER, CFG_VIB_CART, CFG_VIB_XFER, CFG_VIB_ALERT, CFG_VIB_LP,
       CFG_VIB_SDOP };
#define CFG_MAX_ITEMS 22

static bool cfg_is_sep(int id) { return id == CFG_SEP_UI || id == CFG_SEP_MOTOR; }
#define UIEXT_SEP_RULE_COLOR 0xF7BE   // whitesmoke (RGB 245,245,245), both themes

static const char *const caption_labels[] = { "Top", "Mid", "Bottom" };

// Push one config band with the window frame baked into the pixel buffer.
static void draw_cfg_band(int band_idx, const char *label,
                          uint16_t fg, uint16_t bg, int text_x) {
    rasterize_row(menu_row_buf, label, fg, bg, text_x);
    uint8_t bhi = (uint8_t)(UIEXT_COLOR_CFG_BORDER >> 8);
    uint8_t blo = (uint8_t)(UIEXT_COLOR_CFG_BORDER & 0xFF);
    for (int yy = 0; yy < UIEXT_MENU_CHAR_HEIGHT; yy++) {
        int row_base = yy * UIEXT_DISPLAY_WIDTH * 2;
        bool full_row = (band_idx == 0 && yy == 0) ||
                        (band_idx == UIEXT_MENU_VISIBLE_ROWS - 1 &&
                         yy == UIEXT_MENU_CHAR_HEIGHT - 1);
        if (full_row) {
            for (int xx = 0; xx < UIEXT_DISPLAY_WIDTH; xx++) {
                menu_row_buf[row_base + xx * 2]     = bhi;
                menu_row_buf[row_base + xx * 2 + 1] = blo;
            }
        } else {
            menu_row_buf[row_base]     = bhi;
            menu_row_buf[row_base + 1] = blo;
            menu_row_buf[row_base + (UIEXT_DISPLAY_WIDTH - 1) * 2]     = bhi;
            menu_row_buf[row_base + (UIEXT_DISPLAY_WIDTH - 1) * 2 + 1] = blo;
        }
    }
    uint8_t y = (uint8_t)(UIEXT_MENU_ROW_TOP + band_idx * UIEXT_MENU_ROW_PITCH);
    rainbow_stamp(menu_row_buf, y, UIEXT_MENU_CHAR_HEIGHT);
    setAddrWindow(0, y,
                  (uint8_t)(UIEXT_DISPLAY_WIDTH  - 1),
                  (uint8_t)(y + UIEXT_MENU_CHAR_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
    tft_cs_high();
}

// ── Status screen helpers ────────────────────────────────────────────────────
void uiext_ota_screen(const char *title, const char *l1, const char *l2, const char *l3) {
    int title_x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width(title)) / 2;
    if (title_x < UIEXT_MENU_TEXT_X) title_x = UIEXT_MENU_TEXT_X;
    draw_cfg_band(0, title, UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG, title_x);
    const char *lines[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++)
        draw_cfg_band(i + 1, lines[i] ? lines[i] : "",
                      UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                      UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
}

// Band-grid confirm. Only caller is ota_ble.c's forget-bonds prompt, so it is
// radio-gated to keep the Lite image byte-for-byte free of it (acceptance bar
// clause 1: no flash regression).
#if UIEXT_OTA_ENABLED
bool uiext_ota_confirm(const char *title, const char *l1, const char *l2, uint32_t timeout_ms) {
    int title_x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width(title)) / 2;
    if (title_x < UIEXT_MENU_TEXT_X) title_x = UIEXT_MENU_TEXT_X;
    draw_cfg_band(0, title, UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG, title_x);
    draw_cfg_band(1, l1 ? l1 : "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                  UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
    draw_cfg_band(2, l2 ? l2 : "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                  UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);

    bool yes = false;   // default focus on "No" — safe default
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            const int tx = UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD;
            draw_cfg_band(3, "Yes      No",
                          UIEXT_COLOR_CFG_SEL_TEXT, UIEXT_COLOR_CFG_SEL_BG, tx);
            int ux = yes ? tx : tx + (int)label_pixel_width("Yes      ");
            int uw = (int)label_pixel_width(yes ? "Yes" : "No");
            fillRectangle((uint8_t)ux,
                          (uint8_t)(UIEXT_MENU_ROW_TOP + 3 * UIEXT_MENU_ROW_PITCH + 17),
                          (uint8_t)uw, 2, UIEXT_COLOR_CFG_SEL_TEXT);
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0) {
            debounce_button(gpio_get(UIEXT_BTN_UP) == 0 ? UIEXT_BTN_UP : UIEXT_BTN_DOWN);
            yes = !yes;
            redraw = true;
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            debounce_button(UIEXT_BTN_SELECT);
            return yes;
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            debounce_button(UIEXT_BTN_CONFIG);
            return false;
        }
        sleep_ms(10);
    }
}
#endif  // UIEXT_OTA_ENABLED — uiext_ota_confirm

// Scrollable report screen: band 0 = title, bands 1-3 = a 3-line window over
// `lines`; UP/DOWN scrolls, SELECT or K4 exits. Shared by SD Check + System Info.
void uiext_report_view(const char *title, const char **lines, int nlines) {
    int view = 0;
    bool redraw = true;
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(150);
    while (true) {
        if (redraw) {
            redraw = false;
            uiext_ota_screen(title,
                             view + 0 < nlines ? lines[view + 0] : "",
                             view + 1 < nlines ? lines[view + 1] : "",
                             view + 2 < nlines ? lines[view + 2] : "");
        }
        if (gpio_get(UIEXT_BTN_UP) == 0) {
            while (gpio_get(UIEXT_BTN_UP) == 0) sleep_ms(20);
            sleep_ms(100);
            if (view > 0) { view--; redraw = true; }
        } else if (gpio_get(UIEXT_BTN_DOWN) == 0) {
            while (gpio_get(UIEXT_BTN_DOWN) == 0) sleep_ms(20);
            sleep_ms(100);
            if (view < nlines - 3) { view++; redraw = true; }
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0) {
            while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
                sleep_ms(20);
            sleep_ms(150);
            return;
        }
        sleep_ms(10);
    }
}

// ── Centered screens (no button hints) ───────────────────────────────────────
static int centered_x(const char *s) {
    int x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width(s)) / 2;
    return x < 0 ? 0 : x;
}

// Redraw only the centered title bar — cheap enough for the animated ellipsis.
void uiext_ota_title(const char *title) {
    draw_cfg_band(0, title, UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG,
                  centered_x(title));
}

// Centered title + one centered body line; remaining bands blanked.
void uiext_ota_status(const char *title, const char *body) {
    if (!body) body = "";
    uiext_ota_title(title);
    draw_cfg_band(1, "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, UIEXT_MENU_TEXT_X);
    draw_cfg_band(2, body, UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, centered_x(body));
    draw_cfg_band(3, "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, UIEXT_MENU_TEXT_X);
}

// Body layout below the y0-19 title bar.
#define UIEXT_BODY_TOP     20
#define UIEXT_BODY_LINE1_Y 26   // filename / confirm body line (padded below the title)
#define UIEXT_WAIT_NAME_Y  32
#define UIEXT_WAIT_DOTS_Y  52
// Two-line status: the pair block-centered in the body (see uiext_ota_status2)
#define UIEXT_STATUS2_L1_Y 30
#define UIEXT_STATUS2_L2_Y 50

// Blit `text` as a centered CHAR_HEIGHT row at screen y `top` (clears that row).
static void draw_centered_row(const char *text, int top) {
    rasterize_row(menu_row_buf, text, UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, centered_x(text));
    setAddrWindow(0, (uint8_t)top, UIEXT_DISPLAY_WIDTH - 1, (uint8_t)(top + UIEXT_MENU_CHAR_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
    tft_cs_high();
}

// Centered two-line status: title + two adjacent centered lines grouped in the
// middle of the body.
void uiext_ota_status2(const char *title, const char *l1, const char *l2) {
    // The two lines are centered as ONE block in the body below the title
    // band: glyph ink spans rows 31-68 of the 20-79 body, i.e. equal 11 px
    // margins top and bottom (and clear of a bottom rainbow strip at 76).
    uiext_ota_title(title);
    fillRectangle(0, UIEXT_BODY_TOP, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT - UIEXT_BODY_TOP, UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();   // body clear covers the bottom strip positions
    draw_centered_row(l1 ? l1 : "", UIEXT_STATUS2_L1_Y);
    draw_centered_row(l2 ? l2 : "", UIEXT_STATUS2_L2_Y);
}

// Waiting screen: static title, name (line 1), animated ellipsis below it.
void uiext_ota_wait(const char *title, const char *name) {
    if (!name) name = "";
    uiext_ota_title(title);
    fillRectangle(0, UIEXT_BODY_TOP, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT - UIEXT_BODY_TOP, UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();   // body clear covers the bottom strip positions
    draw_centered_row(name, UIEXT_WAIT_NAME_Y);
}

// Title-less wait screen: full theme background + centered caption on the
// waiting line — pair with uiext_wait_anim for the waiting animation.
void uiext_wait_screen(const char *caption) {
    fillScreen(UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();
    draw_centered_row(caption ? caption : "", UIEXT_WAIT_NAME_Y);
}

// Title-less two-line screen: caption on the waiting line plus a second
// centered line in the dots window (error + hint pairs).
void uiext_wait_screen2(const char *caption, const char *line2) {
    uiext_wait_screen(caption);
    draw_centered_row(line2 ? line2 : "", UIEXT_WAIT_DOTS_Y);
}

// ── Braille-cell animations (boot spinner + waiting line) ────────────────────
// Drawn procedurally — the GFX font has no braille glyphs. Each byte is the
// Unicode braille bitmask: bit n = braille dot n+1 (dots 1-3 left column
// top→down, 4-6 right column, 7-8 bottom row). The spinner was ported from
// the Lite branch, replacing the GIF spinner (70 KB flash + 25 KB RAM).
#define SPINNER_DOT_PX   3   // dot square size
#define SPINNER_PITCH_PX 5   // dot grid pitch
#define SPINNER_CELL_PX  (SPINNER_PITCH_PX + SPINNER_DOT_PX)  // one 2×4 cell

// Stamp one braille cell into menu_row_buf at x = gx.
// 2 cols × 4 rows; rows at y 1/6/11/16 fit the 19 px window.
static void braille_cell_stamp(uint8_t bits, int gx) {
    uint8_t fg_hi = (uint8_t)(UIEXT_COLOR_CFG_TEXT >> 8);
    uint8_t fg_lo = (uint8_t)(UIEXT_COLOR_CFG_TEXT & 0xFF);
    for (int d = 0; d < 8; d++) {
        if (!(bits & (1u << d))) continue;
        int col = (d < 6) ? d / 3 : d - 6;
        int row = (d < 6) ? d % 3 : 3;
        int x0 = gx + col * SPINNER_PITCH_PX;
        int y0 = 1 + row * SPINNER_PITCH_PX;
        for (int yy = 0; yy < SPINNER_DOT_PX; yy++)
            for (int xx = 0; xx < SPINNER_DOT_PX; xx++) {
                int idx = ((y0 + yy) * UIEXT_DISPLAY_WIDTH + (x0 + xx)) * 2;
                menu_row_buf[idx]     = fg_hi;
                menu_row_buf[idx + 1] = fg_lo;
            }
    }
}

// Vertically-centered caption+animation pair (title-less waiting screens):
// caption glyphs land at rows 21-35 and the animation at 42-59, so the two
// read as ONE block centered on the 80 px panel (21 px margins both sides).
#define UIEXT_WAIT_PAIR_NAME_Y 17
#define UIEXT_WAIT_PAIR_ANIM_Y 41

// Shared blit for the animations at screen row y_top (titled screens keep the
// classic spot 4 px below the old ellipsis line — grid bottom at y 74, clear
// of a bottom rainbow strip at 76); the block is centered in the window.
static void wait_anim_blit(uint8_t y_top) {
    const uint8_t h = UIEXT_MENU_CHAR_HEIGHT - 1;
    const uint8_t bx = 56, bw = 48;
    setAddrWindow(bx, y_top, bx + bw - 1, y_top + h - 1);
    tft_dc_high();
    tft_cs_low();
    for (uint8_t rr = 0; rr < h; rr++)
        spi_write_blocking(SPI_TFT_PORT, &menu_row_buf[rr * UIEXT_DISPLAY_WIDTH * 2 + bx * 2], (size_t)bw * 2);
    tft_cs_high();
}

// Boot spinner: cli-spinners "dots" (⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏), one cell, 80 ms.
static const uint8_t spinner_frames[] =
    { 0x0B, 0x19, 0x39, 0x38, 0x3C, 0x34, 0x26, 0x27, 0x07, 0x0F };

void uiext_boot_spinner(int frame) {
    uint8_t bits = spinner_frames[frame % (int)sizeof(spinner_frames)];
    rasterize_row(menu_row_buf, "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, 0);  // bg fill
    braille_cell_stamp(bits, (UIEXT_DISPLAY_WIDTH - SPINNER_CELL_PX) / 2);
    wait_anim_blit(UIEXT_WAIT_DOTS_Y + 4);
}

// Waiting-line animation: cli-spinners "dots12" (two cells, 56 frames,
// UIEXT_WAIT_ANIM_MS cadence) — replaces the old "..." ellipsis in the same
// window. Each frame is the {left, right} cell bitmask pair.
static const uint8_t dots12_frames[][2] = {
    {0x80,0x00},{0x40,0x00},{0x04,0x00},{0x82,0x00},{0x42,0x00},{0x05,0x00},
    {0x83,0x00},{0x43,0x00},{0x0D,0x00},{0x8B,0x00},{0x4B,0x00},{0x0D,0x01},
    {0x8B,0x01},{0x4B,0x01},{0x0D,0x09},{0x0B,0x09},{0x0B,0x09},{0x09,0x19},
    {0x09,0x19},{0x09,0x29},{0x08,0x99},{0x08,0x59},{0x88,0x29},{0x40,0x99},
    {0x04,0x59},{0x82,0x29},{0x42,0x98},{0x05,0x58},{0x83,0x28},{0x43,0x90},
    {0x0D,0x50},{0x8B,0x20},{0x4B,0x80},{0x0D,0x41},{0x8B,0x01},{0x4B,0x01},
    {0x0D,0x09},{0x0B,0x09},{0x0B,0x09},{0x09,0x19},{0x09,0x19},{0x09,0x29},
    {0x08,0x99},{0x08,0x59},{0x08,0x29},{0x00,0x99},{0x00,0x59},{0x00,0x29},
    {0x00,0x98},{0x00,0x58},{0x00,0x28},{0x00,0x90},{0x00,0x50},{0x00,0x20},
    {0x00,0x80},{0x00,0x40},
};
_Static_assert(sizeof(dots12_frames) / 2 == UIEXT_WAIT_ANIM_FRAMES,
               "dots12 frame count out of sync with UIEXT_WAIT_ANIM_FRAMES");

static void wait_anim_frame(int frame, uint8_t y_top) {
    const uint8_t *f = dots12_frames[frame % UIEXT_WAIT_ANIM_FRAMES];
    rasterize_row(menu_row_buf, "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, 0);  // bg fill
    const int gx = (UIEXT_DISPLAY_WIDTH - (2 * SPINNER_CELL_PX + 4)) / 2;
    braille_cell_stamp(f[0], gx);
    braille_cell_stamp(f[1], gx + SPINNER_CELL_PX + 4);
    wait_anim_blit(y_top);
}

// Pair position — use with uiext_wait_anim_screen (caption + animation
// centered as one block).
void uiext_wait_anim(int frame) {
    wait_anim_frame(frame, UIEXT_WAIT_PAIR_ANIM_Y);
}

// Classic position below a title band / hinted layout (boot chrome spot).
void uiext_ota_wait_anim(int frame) {
    wait_anim_frame(frame, UIEXT_WAIT_DOTS_Y + 4);
}

// Title-less waiting screen for the caption+animation pair: caption drawn at
// the pair position — animate with uiext_wait_anim.
void uiext_wait_anim_screen(const char *caption) {
    fillScreen(UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();
    draw_centered_row(caption ? caption : "", UIEXT_WAIT_PAIR_NAME_Y);
}

// Selectable band menu (System Tools style): title band 0, bands 1-3 = a
// scrolling window over `items`. UP/DOWN moves, SELECT returns the picked
// index, K4 returns -1.
int uiext_menu_pick(const char *title, const char **items, int n) {
    int sel = 0, view = 0;
    while (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0)
        sleep_ms(20);
    sleep_ms(150);
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            if (sel < view) view = sel;
            if (sel > view + 2) view = sel - 2;
            int title_x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width(title)) / 2;
            if (title_x < UIEXT_MENU_TEXT_X) title_x = UIEXT_MENU_TEXT_X;
            draw_cfg_band(0, title, UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG, title_x);
            for (int row = 0; row < 3; row++) {
                int idx = view + row;
                bool selected = (idx == sel && idx < n);
                draw_cfg_band(row + 1, idx < n ? items[idx] : "",
                              selected ? UIEXT_COLOR_CFG_SEL_TEXT : UIEXT_COLOR_CFG_TEXT,
                              selected ? UIEXT_COLOR_CFG_SEL_BG   : UIEXT_COLOR_CFG_BG,
                              UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
            }
        }
        if (gpio_get(UIEXT_BTN_UP) == 0) {
            debounce_button(UIEXT_BTN_UP);
            if (sel > 0) { sel--; redraw = true; }
        } else if (gpio_get(UIEXT_BTN_DOWN) == 0) {
            debounce_button(UIEXT_BTN_DOWN);
            if (sel < n - 1) { sel++; redraw = true; }
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            debounce_button(UIEXT_BTN_SELECT);
            return sel;
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            debounce_button(UIEXT_BTN_CONFIG);
            return -1;
        }
        sleep_ms(10);
    }
}

// Confirm on the transfer-screen layout: centered title, centered body line,
// and a centered Yes / No selector; the current choice is underlined. UP/DOWN
// moves, SELECT confirms, K4 = No; default focus is "No" (safe).
bool uiext_ota_confirm_centered(const char *title, const char *line, uint32_t timeout_ms) {
    static const char *sel_text = "Yes      No";
    const int sel_y = 48;   // counter region, below the body line
    uiext_ota_title(title);
    fillRectangle(0, UIEXT_BODY_TOP, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT - UIEXT_BODY_TOP, UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();   // body clear covers the bottom strip positions
    draw_centered_row(line ? line : "", UIEXT_BODY_LINE1_Y);
    const int cx    = centered_x(sel_text);
    const int yes_w = (int)label_pixel_width("Yes");
    const int no_x  = cx + (int)label_pixel_width("Yes      ");
    const int no_w  = (int)label_pixel_width("No");

    bool yes = false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            rasterize_row(menu_row_buf, sel_text,
                          UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, cx);
            int ux = yes ? cx : no_x;
            int uw = yes ? yes_w : no_w;
            uint8_t uhi = (uint8_t)(UIEXT_COLOR_CFG_TEXT >> 8);
            uint8_t ulo = (uint8_t)(UIEXT_COLOR_CFG_TEXT & 0xFF);
            for (int py = 17; py <= 18; py++)
                for (int px = ux; px < ux + uw && px < UIEXT_DISPLAY_WIDTH; px++) {
                    int idx = (py * UIEXT_DISPLAY_WIDTH + px) * 2;
                    menu_row_buf[idx]     = uhi;
                    menu_row_buf[idx + 1] = ulo;
                }
            setAddrWindow(0, (uint8_t)sel_y,
                          UIEXT_DISPLAY_WIDTH - 1, (uint8_t)(sel_y + UIEXT_MENU_CHAR_HEIGHT - 1));
            tft_dc_high();
            tft_cs_low();
            spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
            tft_cs_high();
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0) {
            debounce_button(gpio_get(UIEXT_BTN_UP) == 0 ? UIEXT_BTN_UP : UIEXT_BTN_DOWN);
            yes = !yes;
            redraw = true;
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            debounce_button(UIEXT_BTN_SELECT);
            return yes;
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            debounce_button(UIEXT_BTN_CONFIG);
            return false;
        }
        sleep_ms(10);
    }
}

// Two-line Yes/No confirm on the band grid (title + two centered body lines
// + Yes/No band; default focus No). Unlike the bool confirms this reports an
// explicit No separately from a timeout, so callers can treat "user said no"
// and "nobody answered" differently (update decline vs. walk-away).
uiext_confirm_res_t uiext_ota_confirm2(const char *title, const char *l1,
                                       const char *l2, uint32_t timeout_ms) {
    uiext_ota_title(title);
    draw_cfg_band(1, l1 ? l1 : "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                  centered_x(l1 ? l1 : ""));
    draw_cfg_band(2, l2 ? l2 : "", UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                  centered_x(l2 ? l2 : ""));

    static const char *sel_text = "Yes      No";
    bool yes = false;   // default focus on "No" — safe default
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            // Plain background like the centered confirms (owner): the
            // underline alone marks the choice — the selection-highlight
            // bar made this dialog look different for no reason.
            const int tx = centered_x(sel_text);
            draw_cfg_band(3, sel_text,
                          UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, tx);
            int ux = yes ? tx : tx + (int)label_pixel_width("Yes      ");
            int uw = (int)label_pixel_width(yes ? "Yes" : "No");
            fillRectangle((uint8_t)ux,
                          (uint8_t)(UIEXT_MENU_ROW_TOP + 3 * UIEXT_MENU_ROW_PITCH + 17),
                          (uint8_t)uw, 2, UIEXT_COLOR_CFG_TEXT);
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return UIEXT_CONFIRM_TIMEOUT;
        if (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0) {
            debounce_button(gpio_get(UIEXT_BTN_UP) == 0 ? UIEXT_BTN_UP : UIEXT_BTN_DOWN);
            yes = !yes;
            redraw = true;
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            debounce_button(UIEXT_BTN_SELECT);
            return yes ? UIEXT_CONFIRM_YES : UIEXT_CONFIRM_NO;
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            debounce_button(UIEXT_BTN_CONFIG);
            return UIEXT_CONFIRM_NO;
        }
        sleep_ms(10);
    }
}

// Tri-state variant of the centered confirm (title + one centered body line
// + Yes/No directly below, no gap band): reports an explicit No separately
// from a timeout, like uiext_ota_confirm2.
uiext_confirm_res_t uiext_ota_confirm_centered2(const char *title, const char *line,
                                                uint32_t timeout_ms) {
    static const char *sel_text = "Yes      No";
    const int sel_y = 48;   // counter region, below the body line
    uiext_ota_title(title);
    fillRectangle(0, UIEXT_BODY_TOP, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT - UIEXT_BODY_TOP, UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();   // body clear covers the bottom strip positions
    draw_centered_row(line ? line : "", UIEXT_BODY_LINE1_Y);
    const int cx    = centered_x(sel_text);
    const int yes_w = (int)label_pixel_width("Yes");
    const int no_x  = cx + (int)label_pixel_width("Yes      ");
    const int no_w  = (int)label_pixel_width("No");

    bool yes = false;   // default focus on "No" — safe default
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    bool redraw = true;
    while (true) {
        if (redraw) {
            redraw = false;
            rasterize_row(menu_row_buf, sel_text,
                          UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG, cx);
            int ux = yes ? cx : no_x;
            int uw = yes ? yes_w : no_w;
            uint8_t uhi = (uint8_t)(UIEXT_COLOR_CFG_TEXT >> 8);
            uint8_t ulo = (uint8_t)(UIEXT_COLOR_CFG_TEXT & 0xFF);
            for (int py = 17; py <= 18; py++)
                for (int px = ux; px < ux + uw && px < UIEXT_DISPLAY_WIDTH; px++) {
                    int idx = (py * UIEXT_DISPLAY_WIDTH + px) * 2;
                    menu_row_buf[idx]     = uhi;
                    menu_row_buf[idx + 1] = ulo;
                }
            setAddrWindow(0, (uint8_t)sel_y,
                          UIEXT_DISPLAY_WIDTH - 1, (uint8_t)(sel_y + UIEXT_MENU_CHAR_HEIGHT - 1));
            tft_dc_high();
            tft_cs_low();
            spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
            tft_cs_high();
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
            return UIEXT_CONFIRM_TIMEOUT;
        if (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0) {
            debounce_button(gpio_get(UIEXT_BTN_UP) == 0 ? UIEXT_BTN_UP : UIEXT_BTN_DOWN);
            yes = !yes;
            redraw = true;
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            debounce_button(UIEXT_BTN_SELECT);
            return yes ? UIEXT_CONFIRM_YES : UIEXT_CONFIRM_NO;
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            debounce_button(UIEXT_BTN_CONFIG);
            return UIEXT_CONFIRM_NO;
        }
        sleep_ms(10);
    }
}

// Big counter: percentage drawn at NUM/DEN (=1.5x) into a region below the
// filename, horizontally centered.
#define UIEXT_PCT_NUM   3
#define UIEXT_PCT_DEN   2
#define UIEXT_PCT_Y     48
#define UIEXT_PCT_H     32
static uint8_t pct_buf[UIEXT_DISPLAY_WIDTH * UIEXT_PCT_H * 2];

// 2-bit antialiased level of glyph pixel (sx,sy) — random access into the
// row-major packed bitmap (4 pixels per byte, top bits first).
static uint8_t glyph_level(const uint8_t *bm, uint16_t bo, int gw, int sx, int sy) {
    int i = sy * gw + sx;
    return (uint8_t)((bm[bo + (i >> 2)] >> (6 - 2 * (i & 3))) & 0x03);
}

// Render `label` scaled by num/den into `buf` with nearest-neighbour sampling.
static void rasterize_scaled(uint8_t *buf, int height, const char *label,
                             uint16_t fg, uint16_t bg, int cursor_x,
                             int baseline, int num, int den) {
    uint8_t bg_hi = (uint8_t)(bg >> 8), bg_lo = (uint8_t)(bg & 0xFF);
    for (int i = 0; i < UIEXT_DISPLAY_WIDTH * height * 2; i += 2) { buf[i] = bg_hi; buf[i + 1] = bg_lo; }
    if (!label || !label[0]) return;
    uint8_t *bm = _gfxFont->bitmap;
    for (const char *p = label; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < _gfxFont->first || c > _gfxFont->last) continue;
        GFXglyph *g = &_gfxFont->glyph[c - _gfxFont->first];
        int gw = g->width, gh = g->height;
        if (gw > 0 && gh > 0) {
            uint16_t bo = g->bitmapOffset;
            int dw = (gw * num) / den, dh = (gh * num) / den;
            int ox = cursor_x + ((int)g->xOffset * num) / den;
            int oy = baseline + ((int)g->yOffset * num) / den;
            for (int dy = 0; dy < dh; dy++) {
                int py = oy + dy;
                if (py < 0 || py >= height) continue;
                int sy = (dy * den) / num;
                if (sy >= gh) sy = gh - 1;
                for (int dx = 0; dx < dw; dx++) {
                    int sx = (dx * den) / num;
                    if (sx >= gw) sx = gw - 1;
                    uint8_t level = glyph_level(bm, bo, gw, sx, sy);
                    if (!level) continue;
                    int px = ox + dx;
                    if (px < 0 || px >= UIEXT_DISPLAY_WIDTH) continue;
                    uint16_t col = blend_rgb565(fg, bg, level);
                    int idx = (py * UIEXT_DISPLAY_WIDTH + px) * 2;
                    buf[idx] = (uint8_t)(col >> 8);
                    buf[idx + 1] = (uint8_t)(col & 0xFF);
                }
            }
        }
        cursor_x += ((int)g->xAdvance * num) / den;
    }
}

// Render `text` at the counter scale (1.5x), horizontally centered.
void uiext_ota_big_text(const char *text, int y_top) {
    int w = (int)label_pixel_width(text) * UIEXT_PCT_NUM / UIEXT_PCT_DEN;
    int cx = (UIEXT_DISPLAY_WIDTH - w) / 2;
    if (cx < 0) cx = 0;
    int baseline = 2 + (UIEXT_MENU_BASELINE_OFFSET * UIEXT_PCT_NUM) / UIEXT_PCT_DEN;
    rasterize_scaled(pct_buf, UIEXT_PCT_H, text, UIEXT_COLOR_CFG_TEXT, UIEXT_COLOR_CFG_BG,
                     cx, baseline, UIEXT_PCT_NUM, UIEXT_PCT_DEN);
    rainbow_stamp(pct_buf, y_top, UIEXT_PCT_H);   // bottom strip overlaps this region
    const uint8_t bx = 8, bw = UIEXT_DISPLAY_WIDTH - 16;
    setAddrWindow(bx, (uint8_t)y_top, bx + bw - 1, (uint8_t)(y_top + UIEXT_PCT_H - 1));
    tft_dc_high();
    tft_cs_low();
    for (uint8_t rr = 0; rr < UIEXT_PCT_H; rr++)
        spi_write_blocking(SPI_TFT_PORT,
                           &pct_buf[rr * UIEXT_DISPLAY_WIDTH * 2 + bx * 2],
                           (size_t)bw * 2);
    tft_cs_high();
}

// Update ONLY the counter window — never the whole screen.
void uiext_ota_progress_update(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char pctstr[8];
    snprintf(pctstr, sizeof(pctstr), "%d%%", pct);
    uiext_ota_big_text(pctstr, UIEXT_PCT_Y);
}

// Draw the full transfer screen once, then use uiext_ota_progress_update().
void uiext_ota_progress(const char *verb, const char *name, int pct) {
    uiext_ota_title(verb);
    fillRectangle(0, UIEXT_BODY_TOP, UIEXT_DISPLAY_WIDTH, UIEXT_DISPLAY_HEIGHT - UIEXT_BODY_TOP, UIEXT_COLOR_CFG_BG);
    uiext_draw_rainbow();   // body clear covers the bottom strip positions
    draw_centered_row(name ? name : "", UIEXT_BODY_LINE1_Y);
    uiext_ota_progress_update(pct);
}

// Returns true when a Connect session changed the SD (push/delete/rename…) —
// the caller must rebuild its listing, which is stale.
static bool run_config_menu(void) {
    int sel = 0;
    int view = 0;                // first item shown on band 1
    bool running = true;
    bool sd_dirty = false;
    bool opts_dirty = false;     // display settings changed -> save on exit
    // SD Check needs a card: probe once on entry (a mount now forces a full
    // card init, too slow to repeat on every redraw). A card inserted while
    // the menu is open shows up the next time Tools is opened.
    bool sd_present = (sd_fs_mount() == FR_OK);

    while (running) {
        // Build the visible item list (Revert only when a previous FW exists)
        int ids[CFG_MAX_ITEMS];
        int count = 0;
        // Order: BLE (most used first), firmware maintenance, diagnostics,
        // settings, Exit.
#if UIEXT_OTA_ENABLED
        // Hard rule: radio modes (and their flash writes — bonds, OTA slots)
        // only with the cartridge ejected and the QL not using the drive.
        // From the cartridge-ready System Tools these entries simply vanish.
        if (cfInserted == NONE && !mdInUse) {
            ids[count++] = CFG_CONNECT;
            ids[count++] = CFG_PAIR;
            ids[count++] = CFG_PAIRED;
            ids[count++] = CFG_OTA;
            if (ota_other_slot_has_image()) ids[count++] = CFG_REVERT;
        }
#else
        // Lite has no A/B slots: BOOTSEL is one of its two update routes (the
        // other is the SD updater). Stays visible with a cartridge mounted —
        // selecting it then explains the gate, per the owner's BOOTSEL-style
        // "visible but explains" ruling.
        ids[count++] = CFG_BOOTSEL;
#endif
        if (sd_present) ids[count++] = CFG_SDCHECK;
        ids[count++] = CFG_SYSINFO;
        ids[count++] = CFG_LEDTEST;
        ids[count++] = CFG_SEP_UI;   // UI options grouped below the separator
        ids[count++] = CFG_CAPTION;
        ids[count++] = CFG_THEME;
        ids[count++] = CFG_PATHBAR;
        ids[count++] = CFG_RAINBOW;
#if UIEXT_VIBRO_ENABLED
        ids[count++] = CFG_SEP_MOTOR;   // haptic-event toggles
        ids[count++] = CFG_VIB_MASTER;
        ids[count++] = CFG_VIB_CART;
        ids[count++] = CFG_VIB_XFER;
        ids[count++] = CFG_VIB_ALERT;
        ids[count++] = CFG_VIB_LP;
        ids[count++] = CFG_VIB_SDOP;
#endif
        ids[count++] = CFG_EXIT;
        if (sel >= count) sel = count - 1;
        if (sel < view) view = sel;
        if (sel > view + 2) view = sel - 2;

        // Band 0: inverted title bar, centred
        int title_x = (UIEXT_DISPLAY_WIDTH - (int)label_pixel_width("System Tools")) / 2;
        draw_cfg_band(0, "System Tools",
                      UIEXT_COLOR_CFG_TITLE_TEXT, UIEXT_COLOR_CFG_TITLE_BG, title_x);

        // Bands 1..3: scrolling window over the items
        for (int row = 0; row < 3; row++) {
            int idx = view + row;
            char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
            label[0] = '\0';
            if (idx < count) {
                switch (ids[idx]) {
                    case CFG_CAPTION:
                        snprintf(label, sizeof(label), "Caption: %s", caption_labels[g_caption_pos]);
                        break;
                    case CFG_THEME:
                        snprintf(label, sizeof(label), "%s Mode",
                                 uiext_theme_is_dark() ? "Dark" : "Light");
                        break;
                    case CFG_PATHBAR:
                        snprintf(label, sizeof(label), "Path Bar: %s",
                                 g_path_title_enabled ? "On" : "Off");
                        break;
                    case CFG_RAINBOW:
                        snprintf(label, sizeof(label), "Rainbow: %s",
                                 rainbow_labels[s_rainbow_pos]);
                        break;
                    case CFG_CONNECT: snprintf(label, sizeof(label), "Connect");     break;
                    case CFG_OTA:     snprintf(label, sizeof(label), "Update Firmware"); break;
                    case CFG_PAIR:    snprintf(label, sizeof(label), "Pair Device"); break;
                    case CFG_PAIRED:  snprintf(label, sizeof(label), "Paired Devices"); break;
                    case CFG_REVERT:  snprintf(label, sizeof(label), "Revert Firmware"); break;
                    case CFG_SDCHECK: snprintf(label, sizeof(label), "SD Check");    break;
                    case CFG_SYSINFO: snprintf(label, sizeof(label), "System Info"); break;
                    case CFG_LEDTEST: snprintf(label, sizeof(label), "LED & Motor Test"); break;
#if !UIEXT_OTA_ENABLED
                    case CFG_BOOTSEL: snprintf(label, sizeof(label), "BOOTSEL Mode"); break;
#endif
                    case CFG_EXIT:    snprintf(label, sizeof(label), "Exit");        break;
                    case CFG_SEP_UI:
                        // Left-aligned like the items; dotted rules follow
                        strcpy(label, "UI Options");
                        break;
                    case CFG_SEP_MOTOR:
                        strcpy(label, "Motor");
                        break;
                    case CFG_VIB_MASTER:
                        snprintf(label, sizeof(label), "Motor: %s",
                                 uiext_vibro_master_get() ? "On" : "Off");
                        break;
                    case CFG_VIB_CART:
                        snprintf(label, sizeof(label), "Cart: %s",
                                 uiext_vibro_ev_get(UIEXT_VEV_CART) ? "On" : "Off");
                        break;
                    case CFG_VIB_XFER:
                        snprintf(label, sizeof(label), "Transfer: %s",
                                 uiext_vibro_ev_get(UIEXT_VEV_XFER) ? "On" : "Off");
                        break;
                    case CFG_VIB_ALERT:
                        snprintf(label, sizeof(label), "Alerts: %s",
                                 uiext_vibro_ev_get(UIEXT_VEV_ALERT) ? "On" : "Off");
                        break;
                    case CFG_VIB_LP:
                        snprintf(label, sizeof(label), "Long Press: %s",
                                 uiext_vibro_ev_get(UIEXT_VEV_LONGPRESS) ? "On" : "Off");
                        break;
                    case CFG_VIB_SDOP:
                        snprintf(label, sizeof(label), "Load/Save: %s",
                                 sdop_labels[s_vibro_sdop_pos]);
                        break;
                }
            }
            bool is_sep = (idx < count && cfg_is_sep(ids[idx]));
            bool selected = (idx == sel && idx < count);
            draw_cfg_band(row + 1, label,
                          selected ? UIEXT_COLOR_CFG_SEL_TEXT : UIEXT_COLOR_CFG_TEXT,
                          selected ? UIEXT_COLOR_CFG_SEL_BG   : UIEXT_COLOR_CFG_BG,
                          UIEXT_MENU_TEXT_X + UIEXT_CFG_TEXT_PAD);
            if (is_sep) {
                // Whitesmoke dotted rules over the full screen width, one
                // above and one below the text (2 px dot, 2 px gap).
                uint8_t by = (uint8_t)((row + 1) * UIEXT_MENU_ROW_PITCH);
                for (int x = 0; x < UIEXT_DISPLAY_WIDTH; x += 4) {
                    fillRectangle((uint8_t)x, (uint8_t)(by + 2),  2, 1, UIEXT_SEP_RULE_COLOR);
                    fillRectangle((uint8_t)x, (uint8_t)(by + 17), 2, 1, UIEXT_SEP_RULE_COLOR);
                }
            }
        }

        // Wait for a button
        bool handled = false;
        while (!handled) {
            if (gpio_get(UIEXT_BTN_UP) == 0) {
                debounce_button(UIEXT_BTN_UP);
                if (sel > 0) sel--;
                if (cfg_is_sep(ids[sel]) && sel > 0) sel--;   // hop the separator
                handled = true;
            } else if (gpio_get(UIEXT_BTN_DOWN) == 0) {
                debounce_button(UIEXT_BTN_DOWN);
                if (sel < count - 1) sel++;
                if (cfg_is_sep(ids[sel]) && sel < count - 1) sel++;   // hop the separator
                handled = true;
            } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
                uint32_t t_press = to_ms_since_boot(get_absolute_time());
                bool long_press = wait_press_type(UIEXT_BTN_SELECT, t_press);
                if (long_press) {   // drain held button
                    while (gpio_get(UIEXT_BTN_SELECT) == 0) sleep_ms(20);
                    sleep_ms(150);
                }
                (void)long_press;   // only used by the OTA pair entry
                switch (ids[sel]) {
                    case CFG_CAPTION:
                        g_caption_pos = (caption_pos_t)((g_caption_pos + 1) % CAPTION_POS_COUNT);
                        opts_dirty = true;
                        break;
                    case CFG_THEME:
                        uiext_theme_set_dark(!uiext_theme_is_dark());
                        opts_dirty = true;
                        break;
                    case CFG_PATHBAR:
                        g_path_title_enabled = !g_path_title_enabled;
                        opts_dirty = true;
                        break;
                    case CFG_RAINBOW:
                        // Repeated SELECT cycles TL -> TR -> BL -> BR -> Off.
                        s_rainbow_pos = (rainbow_pos_t)((s_rainbow_pos + 1) % (RB_OFF + 1));
                        opts_dirty = true;
                        break;
#if UIEXT_VIBRO_ENABLED
                    case CFG_VIB_MASTER:
                        uiext_vibro_master_set(!uiext_vibro_master_get());
                        opts_dirty = true;
                        break;
                    case CFG_VIB_CART:
                        uiext_vibro_ev_set(UIEXT_VEV_CART, !uiext_vibro_ev_get(UIEXT_VEV_CART));
                        opts_dirty = true;
                        break;
                    case CFG_VIB_XFER:
                        uiext_vibro_ev_set(UIEXT_VEV_XFER, !uiext_vibro_ev_get(UIEXT_VEV_XFER));
                        opts_dirty = true;
                        break;
                    case CFG_VIB_ALERT:
                        uiext_vibro_ev_set(UIEXT_VEV_ALERT, !uiext_vibro_ev_get(UIEXT_VEV_ALERT));
                        opts_dirty = true;
                        break;
                    case CFG_VIB_LP:
                        uiext_vibro_ev_set(UIEXT_VEV_LONGPRESS, !uiext_vibro_ev_get(UIEXT_VEV_LONGPRESS));
                        opts_dirty = true;
                        break;
                    case CFG_VIB_SDOP:
                        // Repeated SELECT cycles Off -> 250ms -> 500ms -> 1s.
                        s_vibro_sdop_pos = (s_vibro_sdop_pos + 1) % SDOP_POS_COUNT;
                        opts_dirty = true;
                        break;
#endif
#if UIEXT_OTA_ENABLED
                    case CFG_CONNECT:
                        ota_run_connect_mode();
                        if (ota_sd_changed()) { sd_dirty = true; running = false; }
                        break;
                    case CFG_OTA:
                        ota_run_update_mode();
                        break;
                    case CFG_PAIR:
                        if (long_press) ota_run_forget_bonds();
                        else            ota_run_pair_mode();
                        break;
                    case CFG_PAIRED:
                        ota_run_paired_list();
                        break;
                    case CFG_REVERT:
                        ota_run_revert();
                        break;
#endif
                    case CFG_SDCHECK:
                        sd_check_run();
                        break;
                    case CFG_SYSINFO:
                        sys_info_run();
                        break;
#if !UIEXT_OTA_ENABLED
                    case CFG_BOOTSEL:
                        // Hard rule: reset_usb_boot kills the firmware with
                        // no eject path — never with a cartridge mounted or
                        // the QL using the drive.
                        if (cfInserted != NONE || mdInUse) {
                            uiext_ota_status("BOOTSEL", "Eject cartridge first");
                            sleep_ms(2000);
                            break;
                        }
                        if (uiext_ota_confirm_centered("BOOTSEL?", "Reboot for USB",
                                                       UIEXT_CONFIRM_TIMEOUT_MS)) {
                            if (opts_dirty) config_save_settings();
                            // Drawn before the reset so the frozen screen says
                            // what state the device is in.
                            uiext_ota_status("BOOTSEL", "Drop UF2 on RPI-RP2");
                            reset_usb_boot(0, 0);
                        }
                        break;
#endif
                    case CFG_LEDTEST: {
                        // Bench probe for the activity LED (GP10 / UI_LD_ACTIVITY)
                        // and the vibration motor (J3/GP11): re-inits the pads and
                        // drives them directly, so it answers "are the LED and
                        // motor paths alive?" independently of the hot-plug pin
                        // setup and of the UI state machine.
                        uiext_ota_status("LED & Motor Test", "Blink+buzz 5s");
                        gpio_init(PIN_LED_ACTIVITY);
                        gpio_set_dir(PIN_LED_ACTIVITY, GPIO_OUT);
#if UIEXT_VIBRO_ENABLED
                        vibro_start();
#endif
                        uint32_t until = to_ms_since_boot(get_absolute_time()) + 5000;
                        uint32_t now;
                        while ((now = to_ms_since_boot(get_absolute_time())) < until) {
                            gpio_put(PIN_LED_ACTIVITY, (now / 250) & 1);  // 2 Hz
                            sleep_ms(10);
                        }
#if UIEXT_VIBRO_ENABLED
                        vibro_stop();
#endif
                        gpio_put(PIN_LED_ACTIVITY, 0);
                        break;
                    }
                    case CFG_EXIT:
                        running = false;
                        break;
                }
                handled = true;
            } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
                debounce_button(UIEXT_BTN_CONFIG);
                running = false;
                handled = true;
            }
            sleep_ms(10);
        }
    }

    if (opts_dirty)
        config_save_settings();
    return sd_dirty;
}

void uiext_system_tools(void) {
    (void)run_config_menu();
}

// ── Horizontal scroll helpers ─────────────────────────────────────────────────
#if UIEXT_SCROLL_ANIM_ENABLED

static void full_display_text(const char *src, char *dst, int dst_size) {
    if (src[0] != '[' && strchr(src, '.') == NULL) {
        dst[0] = '<';
        int name_len = (int)strlen(src);
        int max_copy = dst_size - 3;
        if (name_len > max_copy) name_len = max_copy;
        memcpy(dst + 1, src, (size_t)name_len);
        dst[1 + name_len] = '>';
        dst[2 + name_len] = '\0';
    } else {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static bool is_item_clipped(const char *src) {
    char full[UIEXT_SCROLL_MAX_TEXT_CHARS + 3];
    full_display_text(src, full, (int)sizeof(full));
    return label_pixel_width(full) > (UIEXT_DISPLAY_WIDTH - UIEXT_MENU_LIST_TEXT_X);
}

static void draw_hscroll_row(const char *full_text, bool tagged, int scroll_px, uint8_t row_y) {
    rasterize_row(menu_row_buf, full_text,
                  UIEXT_COLOR_SEL_TEXT, UIEXT_COLOR_SEL_BG,
                  UIEXT_MENU_LIST_TEXT_X - scroll_px);
    if (tagged) tag_bar_stamp(menu_row_buf);   // fixed in the gutter, text slides under
    rainbow_stamp(menu_row_buf, row_y, UIEXT_MENU_CHAR_HEIGHT);
    setAddrWindow(0, row_y,
                  (uint8_t)(UIEXT_DISPLAY_WIDTH  - 1),
                  (uint8_t)(row_y + UIEXT_MENU_CHAR_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, menu_row_buf, sizeof(menu_row_buf));
    tft_cs_high();
}

#endif  // UIEXT_SCROLL_ANIM_ENABLED

// ── Vertical scroll animation ─────────────────────────────────────────────────
#if UIEXT_VSCROLL_ANIM_ENABLED

// Scratch buffer holds the sliding rows + 1 spare so the window can slide by
// one full ROW_PITCH without going out of bounds.
static uint8_t vscroll_scratch[UIEXT_DISPLAY_WIDTH * UIEXT_VSCROLL_SCRATCH_HEIGHT * 2];

static bool any_button_pressed(void) {
    return !gpio_get(UIEXT_BTN_UP)     || !gpio_get(UIEXT_BTN_DOWN) ||
           !gpio_get(UIEXT_BTN_SELECT) || !gpio_get(UIEXT_BTN_CONFIG);
}

// Rasterise one menu row (with AA) into the vscroll_scratch buffer.
static void render_row_to_scratch(int scratch_row_idx, const char *label, bool tagged,
                                  bool selected) {
    uint16_t bg = selected ? UIEXT_COLOR_SEL_BG   : UIEXT_COLOR_BG;
    uint16_t fg = selected ? UIEXT_COLOR_SEL_TEXT  : UIEXT_COLOR_TEXT;
    uint8_t *rowbuf = vscroll_scratch + (uint32_t)scratch_row_idx
                          * UIEXT_MENU_ROW_PITCH * UIEXT_DISPLAY_WIDTH * 2;
    rasterize_row(rowbuf, label, fg, bg, UIEXT_MENU_LIST_TEXT_X);
    if (tagged) tag_bar_stamp(rowbuf);
}

// Push the sliding window in one SPI transfer.
static void blit_scratch_window(int read_y) {
    uint8_t y0 = g_path_title_enabled ? UIEXT_MENU_ROW_PITCH : 0;
    uint8_t *win = vscroll_scratch + read_y * UIEXT_DISPLAY_WIDTH * 2;
    int win_h = UIEXT_DISPLAY_HEIGHT - y0;

    // Composite the rainbow into the outgoing frame, saving the touched
    // scratch rows to restore after the send.
    uint8_t saved[RB_H * UIEXT_DISPLAY_WIDTH * 2];
    int saved_rows[RB_H], nsaved = 0;
    int rx0, ry0;
    if (rainbow_geom(&rx0, &ry0)) {
        for (int r = 0; r < RB_H; r++) {
            int sy = ry0 + r;
            if (sy < y0 || sy >= UIEXT_DISPLAY_HEIGHT) continue;
            int wrow = sy - y0;
            memcpy(saved + nsaved * UIEXT_DISPLAY_WIDTH * 2,
                   win + wrow * UIEXT_DISPLAY_WIDTH * 2, UIEXT_DISPLAY_WIDTH * 2);
            saved_rows[nsaved++] = wrow;
        }
        rainbow_stamp(win, y0, win_h);
    }

    setAddrWindow(0, y0,
                  (uint8_t)(UIEXT_DISPLAY_WIDTH  - 1),
                  (uint8_t)(UIEXT_DISPLAY_HEIGHT - 1));
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, win,
                       (uint32_t)win_h * UIEXT_DISPLAY_WIDTH * 2);
    tft_cs_high();

    for (int i = 0; i < nsaved; i++)
        memcpy(win + saved_rows[i] * UIEXT_DISPLAY_WIDTH * 2,
               saved + i * UIEXT_DISPLAY_WIDTH * 2, UIEXT_DISPLAY_WIDTH * 2);
}

static void do_vscroll(char **items, int item_count, int new_offset, int dir) {
    bool title = g_path_title_enabled;
    int nrows           = title ? UIEXT_MENU_VISIBLE_ROWS : UIEXT_MENU_VISIBLE_ROWS + 1;
    int scratch_start   = (dir > 0) ? (title ? new_offset : new_offset - 1)
                                    : (title ? new_offset + 1 : new_offset);
    int sel_scratch_row = (dir > 0) ? (title ? 1 : 2)
                                    : (title ? 0 : 1);

    for (int r = 0; r < nrows; r++) {
        int idx = scratch_start + r;
        char label[UIEXT_MENU_ITEM_MAX_CHARS + 3];
        bool tagged = false;
        if (idx >= 1 && idx < item_count) { // items[0] = unused header slot, never shown
            const char *raw = items[idx];
            tagged = (raw[0] == '*');
            format_menu_item(tagged ? raw + 1 : raw, label);
        } else
            label[0] = '\0';
        render_row_to_scratch(r, label, tagged, (r == sel_scratch_row));
    }

    // Slide a display-height window through the scratch buffer 1 px/frame.
    for (int anim_px = 1; anim_px <= UIEXT_MENU_ROW_PITCH; anim_px++) {
        if (any_button_pressed()) break;
        int read_y = (dir > 0) ? anim_px : (UIEXT_MENU_ROW_PITCH - anim_px);
        blit_scratch_window(read_y);
        sleep_ms(UIEXT_VSCROLL_STEP_MS);
    }

    // Guarantee the correct final state even if animation was cut short.
    blit_scratch_window((dir > 0) ? UIEXT_MENU_ROW_PITCH : 0);
}

#endif  // UIEXT_VSCROLL_ANIM_ENABLED

// ── Main menu loop ────────────────────────────────────────────────────────────
// Band menu over `items` (items[0] = unused header slot). Blocks until SELECT
// (returns 1-based index) or long-SELECT (star-tag preview toggled on the
// highlighted label, returns UIEXT_LONG_SELECT). K4 runs System Tools inline
// and the loop continues. *offset persists the scroll position across calls.
int uiext_menu_run(char **items, int count, int *offset) {
    if (count < 2) return -1;
    if (*offset > count - 2) *offset = count - 2;
    if (*offset < 0) *offset = 0;

    drawMenu(items, count, *offset);

    // Card-removal watch: the listing describes a card that may be gone, so
    // poll cheaply (one SPI status command) and bail out with
    // UIEXT_MENU_SD_GONE so the caller can return to the waiting screen.
    uint32_t next_sd_poll_ms = to_ms_since_boot(get_absolute_time())
                               + UIEXT_SD_POLL_MS;

#if UIEXT_SCROLL_ANIM_ENABLED
    uint32_t last_input_ms = to_ms_since_boot(get_absolute_time());
    int scroll_px  = 0;
    int scroll_dir = 1;
#endif

    while (true) {
        {   // card still there?
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            if (now_ms >= next_sd_poll_ms) {
                next_sd_poll_ms = now_ms + UIEXT_SD_POLL_MS;
                if (!sd_menu_card_present()) return UIEXT_MENU_SD_GONE;
            }
        }
        if (gpio_get(UIEXT_BTN_UP) == 0) {
            uint32_t t_press = to_ms_since_boot(get_absolute_time());
            bool lp = wait_press_type(UIEXT_BTN_UP, t_press);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (lp) {
                while (gpio_get(UIEXT_BTN_UP) == 0) {
                    if (*offset > 0) { (*offset)--; drawMenu(items, count, *offset); }
                    sleep_ms(UIEXT_LONG_PRESS_REPEAT_MS);
                }
#if UIEXT_SCROLL_ANIM_ENABLED
                last_input_ms = to_ms_since_boot(get_absolute_time());
#endif
            } else {
                if (*offset > 0) {
                    (*offset)--;
#if UIEXT_VSCROLL_ANIM_ENABLED
                    // 1→0 with the title off is a highlight hop — no slide.
                    if (!g_path_title_enabled && *offset == 0)
                        drawMenu(items, count, *offset);
                    else
                        do_vscroll(items, count, *offset, -1);
#else
                    drawMenu(items, count, *offset);
#endif
                }
            }
        }

        if (gpio_get(UIEXT_BTN_DOWN) == 0) {
            uint32_t t_press = to_ms_since_boot(get_absolute_time());
            bool lp = wait_press_type(UIEXT_BTN_DOWN, t_press);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (lp) {
                while (gpio_get(UIEXT_BTN_DOWN) == 0) {
                    if (*offset < count - 2) { (*offset)++; drawMenu(items, count, *offset); }
                    sleep_ms(UIEXT_LONG_PRESS_REPEAT_MS);
                }
#if UIEXT_SCROLL_ANIM_ENABLED
                last_input_ms = to_ms_since_boot(get_absolute_time());
#endif
            } else {
                if (*offset < count - 2) {
                    (*offset)++;
#if UIEXT_VSCROLL_ANIM_ENABLED
                    // 0→1 with the title off is a highlight hop — no slide.
                    if (!g_path_title_enabled && *offset == 1)
                        drawMenu(items, count, *offset);
                    else
                        do_vscroll(items, count, *offset, +1);
#else
                    drawMenu(items, count, *offset);
#endif
                }
            }
        }

        if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            uint32_t t0 = to_ms_since_boot(get_absolute_time());
            bool long_detected = false;
            while (gpio_get(UIEXT_BTN_SELECT) == 0) {
                if (!long_detected &&
                    (to_ms_since_boot(get_absolute_time()) - t0) >= (uint32_t)UIEXT_LONG_PRESS_MS) {
                    long_detected = true;
                    uiext_vibrate_event(UIEXT_VEV_LONGPRESS);
                    // Preview at threshold: toggle the * on the highlighted
                    // file label (never on bracketed/dir entries).
                    char *item = items[*offset + 1];
                    if (item[0] != '[' && strrchr(item, '.') != NULL) {
                        if (item[0] == '*') {
                            memmove(item, item + 1, strlen(item));  // remove *
                        } else {
                            memmove(item + 1, item, strlen(item) + 1);  // prepend *
                            item[0] = '*';
                        }
                    }
                    drawMenu(items, count, *offset);
                }
                sleep_ms(10);
            }
            sleep_ms(200);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
            if (long_detected)
                return UIEXT_LONG_SELECT;
            do_select_blink(items[*offset + 1],
                            (!g_path_title_enabled && *offset == 0) ? 0 : 1);
            return *offset + 1;
        }

        if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            uint32_t t_press = to_ms_since_boot(get_absolute_time());
            bool long_press = wait_press_type(UIEXT_BTN_CONFIG, t_press);
#if UIEXT_SCROLL_ANIM_ENABLED
            last_input_ms = to_ms_since_boot(get_absolute_time());
            scroll_px = 0; scroll_dir = 1;
#endif
#if UIEXT_OTA_ENABLED
            // Shortcut fires AT the 1 s threshold, while K4 is still held;
            // Connect mode swallows the initial held button itself. Same
            // radio gate as the menu entries (the browser implies ejected).
            bool sd_dirty;
            if (long_press && cfInserted == NONE && !mdInUse) {
                ota_run_connect_mode();
                sd_dirty = ota_sd_changed();
            } else {
                sd_dirty = run_config_menu();
            }
            if (sd_dirty) return UIEXT_MENU_SD_CHANGED;
#else
            (void)long_press;
            (void)run_config_menu();
#endif
            drawMenu(items, count, *offset);
        }

#if UIEXT_SCROLL_ANIM_ENABLED
        {
            uint32_t now_ms     = to_ms_since_boot(get_absolute_time());
            int      items_left = count - *offset;

            if (items_left > 1 &&
                (now_ms - last_input_ms) >= (uint32_t)UIEXT_SCROLL_ANIM_IDLE_MS) {

                const char *sel = items[*offset + 1];
                bool sel_tagged = (sel[0] == '*');
                if (sel_tagged) sel++;
                if (is_item_clipped(sel)) {
                    char full_text[UIEXT_SCROLL_MAX_TEXT_CHARS + 3];
                    full_display_text(sel, full_text, (int)sizeof(full_text));

                    int text_px_width = (int)label_pixel_width(full_text);
                    int visible_px    = (int)UIEXT_DISPLAY_WIDTH - UIEXT_MENU_LIST_TEXT_X;
                    int max_scroll_px = text_px_width - visible_px;

                    // Only the selected row scrolls; the other rows are static.
                    draw_hscroll_row(full_text, sel_tagged, scroll_px,
                                     (!g_path_title_enabled && *offset == 0)
                                         ? UIEXT_MENU_ROW_TOP
                                         : (uint8_t)(UIEXT_MENU_ROW_TOP + UIEXT_MENU_ROW_PITCH));

                    scroll_px += scroll_dir;
                    if (scroll_px >= max_scroll_px) { scroll_px = max_scroll_px; scroll_dir = -1; }
                    else if (scroll_px <= 0)         { scroll_px = 0;            scroll_dir =  1; }

                    sleep_ms(UIEXT_SCROLL_ANIM_STEP_MS);
                }
            }
        }
#endif
    }
}
