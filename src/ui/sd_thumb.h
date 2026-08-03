// Draws "<basename>.thumb" (24-bit uncompressed BMP) from the current SD
// directory into the preview area, scaled to fit without cropping, flush to
// the top and centred. Returns false if the file is missing/unsupported —
// the caller then draws the plain placeholder.
#pragma once
#include <stdbool.h>

bool sd_thumb_show(const char *mdv_name);
