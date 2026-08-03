// MDV thumbnail renderer, two sidecar conventions (checked in this order):
//
// 1. "<name>.mdv.thumb" (full filename + .thumb) — the app-repo §5b sidecar:
//    raw 160×80 RGB565 big-endian framebuffer, no header, exactly 25,600
//    bytes (any other size = treated as absent). Streamed straight to the
//    panel — the SPI path already takes RGB565 high-byte-first, no swap.
// 2. Legacy "<name>.thumb" (extension replaced): 24-bit uncompressed BMP
//    from tools/make_thumb.py, contain-fit scaled.
//
// Layout contract (see show_selection_preview): the placeholder area is the
// full display, flush to the top and both sides; the image is scaled to FIT
// (contain - never cropped, never bleeding past an edge), anchored to the
// top and centred horizontally.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "hw.h"
#include "ST7735_TFT.h"
#include "UserInterfaceExtension.h"
#include "sd_menu.h"
#include "sd_thumb.h"

#define THUMB_MAX_SRC_W 640          // converter output is <=160 anyway
#define AREA_W UIEXT_DISPLAY_WIDTH   // 160
#define AREA_H UIEXT_DISPLAY_HEIGHT  // 80

static uint8_t s_row[2048];          // one source row (stride <= 640*3+pad)
static uint8_t s_band[AREA_W * 2];   // one destination row, RGB565 BE

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void push_row(int x0, int y, int w) {
    setAddrWindow((uint8_t)x0, (uint8_t)y, (uint8_t)(x0 + w - 1), (uint8_t)y);
    tft_dc_high();
    tft_cs_low();
    spi_write_blocking(SPI_TFT_PORT, s_band, (size_t)w * 2);
    tft_cs_high();
}

// App-repo §5b sidecar: raw 160×80 RGB565 BE, exactly 25,600 bytes. Streamed
// in chunks straight into a full-screen address window — no frame buffer.
static bool show_raw_sidecar(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    const uint32_t total = (uint32_t)AREA_W * AREA_H * 2;
    if (f_size(&f) != (FSIZE_t)total) { f_close(&f); return false; }
    setAddrWindow(0, 0, AREA_W - 1, AREA_H - 1);
    tft_dc_high();
    tft_cs_low();
    bool ok = true;
    uint32_t left = total;
    while (left && ok) {
        UINT want = left > sizeof(s_row) ? (UINT)sizeof(s_row) : (UINT)left;
        UINT n = 0;
        ok = f_read(&f, s_row, want, &n) == FR_OK && n == want;
        if (ok) {
            spi_write_blocking(SPI_TFT_PORT, s_row, n);
            left -= n;
        }
    }
    tft_cs_high();
    f_close(&f);
    if (ok) printf("[thumb] %s: raw 160x80\n", path);
    return ok;
}

bool sd_thumb_show(const char *mdv_name) {
    const char *dirp = sd_menu_cur_path();
    const char *dir = strcmp(dirp, "/") == 0 ? "" : dirp;
    char path[160];

    // 1) "<curdir>/<name.mdv>.thumb" — raw RGB565 sidecar (app §5b convention)
    int w = snprintf(path, sizeof(path), "%s/%s.thumb", dir, mdv_name);
    if (w > 0 && w < (int)sizeof(path) && show_raw_sidecar(path)) return true;

    // 2) legacy "<curdir>/<basename>.thumb" — 24-bit BMP
    const char *dot = strrchr(mdv_name, '.');
    size_t base_len = dot ? (size_t)(dot - mdv_name) : strlen(mdv_name);
    w = snprintf(path, sizeof(path), "%s/%.*s.thumb", dir, (int)base_len, mdv_name);
    if (w < 0 || w >= (int)sizeof(path)) return false;

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;

    // --- BMP header ---------------------------------------------------------
    uint8_t hdr[54];
    UINT n = 0;
    bool ok = f_read(&f, hdr, sizeof(hdr), &n) == FR_OK && n == sizeof(hdr) &&
              hdr[0] == 'B' && hdr[1] == 'M';
    uint32_t data_off = ok ? rd_u32(&hdr[10]) : 0;
    int32_t  src_w    = ok ? (int32_t)rd_u32(&hdr[18]) : 0;
    int32_t  src_h_s  = ok ? (int32_t)rd_u32(&hdr[22]) : 0;
    bool top_down     = src_h_s < 0;
    int32_t  src_h    = top_down ? -src_h_s : src_h_s;
    ok = ok && rd_u16(&hdr[26]) == 1 && rd_u16(&hdr[28]) == 24 &&
         rd_u32(&hdr[30]) == 0 &&                       // BI_RGB only
         src_w > 0 && src_w <= THUMB_MAX_SRC_W &&
         src_h > 0 && src_h <= 4096;
    if (!ok) {
        printf("[thumb] %s: unsupported (need 24-bit uncompressed BMP)\n", path);
        f_close(&f);
        return false;
    }
    uint32_t stride = ((uint32_t)src_w * 3 + 3) & ~3u;

    // --- contain-fit: scale to the largest size that fits 160x80 ------------
    float scale_w = (float)AREA_W / (float)src_w;
    float scale_h = (float)AREA_H / (float)src_h;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    int out_w = (int)((float)src_w * scale + 0.5f);
    int out_h = (int)((float)src_h * scale + 0.5f);
    if (out_w < 1) out_w = 1;
    if (out_w > AREA_W) out_w = AREA_W;
    if (out_h < 1) out_h = 1;
    if (out_h > AREA_H) out_h = AREA_H;
    int x0 = (AREA_W - out_w) / 2;   // centred; flush when full-width
    int y0 = 0;                      // flush to the top

    // --- stream rows, nearest-neighbour scale, draw per dest row ------------
    ok = f_lseek(&f, data_off) == FR_OK;
    for (int32_t fr = 0; ok && fr < src_h; fr++) {
        ok = f_read(&f, s_row, stride, &n) == FR_OK && n == stride;
        if (!ok) break;
        int32_t sy = top_down ? fr : (src_h - 1 - fr);
        int dy_a = (int)((float)sy * scale);
        int dy_b = (int)((float)(sy + 1) * scale);
        if (dy_b > out_h) dy_b = out_h;
        if (dy_a >= dy_b) continue;               // row scaled away
        for (int dx = 0; dx < out_w; dx++) {      // build the dest row once
            int sx = (int)((float)dx / scale);
            if (sx >= src_w) sx = src_w - 1;
            uint8_t b = s_row[sx * 3 + 0], g = s_row[sx * 3 + 1], r = s_row[sx * 3 + 2];
            uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            s_band[dx * 2]     = (uint8_t)(c >> 8);
            s_band[dx * 2 + 1] = (uint8_t)(c & 0xFF);
        }
        for (int dy = dy_a; dy < dy_b; dy++)
            push_row(x0, y0 + dy, out_w);
    }
    f_close(&f);
    if (ok) printf("[thumb] %s: %ldx%ld -> %dx%d\n", path,
                   (long)src_w, (long)src_h, out_w, out_h);
    return ok;
}
