// Roboto Thin — 16 px  |  Adafruit GFX format (2-bit per pixel AA)
// ASCII 0x20–0x7E  |  ascent=15  descent=4  yAdvance=19
// Bitmap: 2-bit coverage levels (0=transparent … 3=opaque), 4 pixels/byte, MSB-first.
// Use rasterize_row() — NOT the library drawChar() which expects 1-bit bitmaps.
#pragma once
#include "ST7735_TFT.h"

extern const GFXfont Roboto_Thin_8;
