// LCD panel probe — dial in the init for an unknown 0.96" 80x160 module.
//
// The firmware hard-codes one panel profile in TFT_80x160_Initialize()
// (src/ui/UserInterfaceExtension.c): inversion ON, MADCTL MX|MV|BGR, and the
// 26/1 start offsets. A different batch of panels needs different values, and
// guessing them one rebuild at a time is slow. This throwaway image puts the
// four variables on the buttons so a panel can be dialled in on the bench:
//
//   K1  inversion on/off          K3  colstart -1  (vertical shift,   cycles 29-22)
//   K2  colour order RGB/BGR      K4  rowstart -1  (horizontal shift, cycles 3-0)
//
// The offsets step DOWN through a short cycle rather than up through 0-31: the
// fault they correct is garbage at the top/left edge, which means the offset is
// too high, and a short cycle means a wrong guess costs a few presses instead
// of wrapping all the way round.
//
// Read the panel, then copy the four values back into the firmware. The
// readout uses a 3x5 bitmap drawn with rectangles rather than the library's
// font path: a panel that is mis-initialised enough to need this tool cannot
// be trusted to render a proportional font legibly.
//
// Built as a RAM-only image (see the target in CMakeLists.txt), so it never
// touches flash: the installed firmware survives and a power cycle brings it
// back, with nothing to reinstall. Swap pico/pico2_w for the board at hand.
//   cmake -S . -B build/probe -G Ninja -DPICO_BOARD=pico -DMPD_BUILD_LCD_PROBE=ON
//   cmake --build build/probe --target lcd_probe
//   picotool load -v -x build/probe/lcd_probe.uf2

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ST7735_TFT.h"
#include "hw.h"
#include "UserInterfaceExtension.h"

// Library-internal panel state, set before setRotation() consumes it.
extern uint8_t _colstart, _rowstart, _tft_type;

// ── Panel profile under test ─────────────────────────────────────────────────
// Defaults are the profile dialled in for the second panel batch, not the
// firmware's current values: that batch wants inversion OFF (every colour bar
// came out complemented with it on) and 24/0 rather than the firmware's 26/1
// (which left a garbage line along the top and left edge). Verified on the
// bench: correct bars, clean frame, clean checkerboard.
static bool    s_invert   = false;  // INVOFF
static bool    s_bgr      = true;   // MADCTL bit 0x08
static uint8_t s_colstart = 24;
static uint8_t s_rowstart = 0;

// Short cycles around the plausible values for an 80x160 module, stepped
// downwards. The GRAM is 132x162, so the 80-wide axis starts somewhere in the
// low twenties and the 160-long axis at or near zero.
#define PROBE_COL_MIN 22
#define PROBE_COL_MAX 29
#define PROBE_ROW_MIN 0
#define PROBE_ROW_MAX 3

static void panel_init(void) {
    tft_dc_low();
    Rcmd1();
    write_command(ST7735_CASET);
    write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x4F);   // 0..79 columns
    write_command(ST7735_RASET);
    write_data(0x00); write_data(0x00);
    write_data(0x00); write_data(0x9F);   // 0..159 rows
    Rcmd3();
    write_command(s_invert ? ST7735_INVON : ST7735_INVOFF);
    _colstart = s_colstart;
    _rowstart = s_rowstart;
    _tft_type = 1;
    setRotation(3);                       // landscape; consumes the offsets
    // setRotation writes MADCTL with the library's RGB order, so the colour
    // order is re-issued here, after it.
    write_command(ST7735_MADCTL);
    write_data((uint8_t)(0x40 | 0x20 | (s_bgr ? 0x08 : 0x00)));  // MX | MV | BGR?
    tft_height = 80;                      // actual panel height
}

// ── 3x5 glyphs: digits plus the four labels, one byte per row, bit 2 = left ──
static const uint8_t GLYPHS[][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
    {7,2,2,2,7},   // 10: I — inversion
    {6,5,6,5,6},   // 11: B — colour order
    {7,4,4,4,7},   // 12: C — colstart
    {6,5,6,5,5},   // 13: R — rowstart
};
#define GLYPH_I 10
#define GLYPH_B 11
#define GLYPH_C 12
#define GLYPH_R 13

#define GLYPH_SCALE 3
#define GLYPH_STEP  (4 * GLYPH_SCALE)   // 3 px glyph + 1 px gap

static uint8_t draw_glyph(uint8_t x, uint8_t y, uint8_t index, uint16_t colour) {
    for (uint8_t row = 0; row < 5; row++)
        for (uint8_t col = 0; col < 3; col++)
            if (GLYPHS[index][row] & (4 >> col))
                fillRect((uint8_t)(x + col * GLYPH_SCALE), (uint8_t)(y + row * GLYPH_SCALE),
                         GLYPH_SCALE, GLYPH_SCALE, colour);
    return (uint8_t)(x + GLYPH_STEP);
}

// The offsets get two digits so 26 and 01 are the same width and the row never
// reflows; the two flags get one, because four 3-glyph fields would be 168 px
// on a 160 px panel and the last one would fall off the edge.
static uint8_t draw_field(uint8_t x, uint8_t y, uint8_t label, uint8_t value,
                          uint8_t digits, uint16_t colour) {
    x = draw_glyph(x, y, label, colour);
    if (digits > 1) x = draw_glyph(x, y, (uint8_t)(value / 10 % 10), colour);
    x = draw_glyph(x, y, (uint8_t)(value % 10), colour);
    return (uint8_t)(x + GLYPH_SCALE * 2);   // field gap
}

static void draw_pattern(void) {
    const uint16_t BLACK = 0x0000, WHITE = 0xFFFF;
    const uint16_t RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F;

    fillScreen(BLACK);

    // 1 px frame on the panel edge. All four sides visible and flush = the
    // offsets are right; a missing side or a wrapped stripe = they are not.
    drawFastHLine(0, 0, 160, WHITE);
    drawFastHLine(0, 79, 160, WHITE);
    drawFastVLine(0, 0, 80, WHITE);
    drawFastVLine(159, 0, 80, WHITE);

    // Colour bars, left to right: red, green, blue, white. Inversion wrong =
    // every bar is its complement (red reads cyan, white reads black);
    // colour order wrong = the red and blue bars trade places.
    const uint16_t bars[4] = { RED, GREEN, BLUE, WHITE };
    for (uint8_t i = 0; i < 4; i++)
        fillRect((uint8_t)(6 + i * 38), 6, 34, 28, bars[i]);

    // Readout: I<inv> B<bgr> C<colstart> R<rowstart>.
    uint8_t x = 8;
    x = draw_field(x, 42, GLYPH_I, (uint8_t)(s_invert ? 1 : 0), 1, WHITE);
    x = draw_field(x, 42, GLYPH_B, (uint8_t)(s_bgr ? 1 : 0), 1, WHITE);
    x = draw_field(x, 42, GLYPH_C, s_colstart, 2, WHITE);
    (void)draw_field(x, 42, GLYPH_R, s_rowstart, 2, WHITE);

    // 2 px checkerboard. Clean squares = the row stride matches the panel;
    // diagonal drift or stripes = it does not, which is a width/MADCTL fault
    // rather than anything these four variables can fix.
    for (uint8_t cy = 0; cy < 7; cy++)
        for (uint8_t cx = 0; cx < 74; cx++)
            if ((cx + cy) & 1)
                fillRect((uint8_t)(6 + cx * 2), (uint8_t)(62 + cy * 2), 2, 2, WHITE);
}

static void wait_release(uint pin) {
    while (gpio_get(pin) == 0) sleep_ms(20);
    sleep_ms(150);
}

int main(void) {
    // No RST net: the panel's RC reset needs time before it takes commands.
    sleep_ms(50);
    spi_init(SPI_TFT_PORT, 40 * 1000 * 1000);
    gpio_set_function(UIEXT_TFT_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(UIEXT_TFT_MOSI, GPIO_FUNC_SPI);
    tft_spi_init();

    const uint buttons[4] = { UIEXT_BTN_UP, UIEXT_BTN_DOWN, UIEXT_BTN_SELECT, UIEXT_BTN_CONFIG };
    for (int i = 0; i < 4; i++) {
        gpio_init(buttons[i]);
        gpio_set_dir(buttons[i], GPIO_IN);
        gpio_pull_up(buttons[i]);
    }

    panel_init();
    draw_pattern();

    while (true) {
        if (gpio_get(UIEXT_BTN_UP) == 0) {
            s_invert = !s_invert;
            wait_release(UIEXT_BTN_UP);
        } else if (gpio_get(UIEXT_BTN_DOWN) == 0) {
            s_bgr = !s_bgr;
            wait_release(UIEXT_BTN_DOWN);
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0) {
            s_colstart = (uint8_t)(s_colstart > PROBE_COL_MIN ? s_colstart - 1
                                                              : PROBE_COL_MAX);
            wait_release(UIEXT_BTN_SELECT);
        } else if (gpio_get(UIEXT_BTN_CONFIG) == 0) {
            s_rowstart = (uint8_t)(s_rowstart > PROBE_ROW_MIN ? s_rowstart - 1
                                                              : PROBE_ROW_MAX);
            wait_release(UIEXT_BTN_CONFIG);
        } else {
            sleep_ms(10);
            continue;
        }
        panel_init();      // offsets are latched at init, so re-run it
        draw_pattern();
    }
}
