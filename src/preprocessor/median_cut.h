#pragma once

#include "common.h"
#include <stdint.h>
#include <stddef.h>

// Median-cut color quantization. Returns up to maxColors ARGB entries with alpha = 0xFF.
// `outPalette` must have room for at least maxColors uint32_t entries. Returns the actual count of palette entries written.
size_t MedianCut_quantize(const uint32_t* pixels, size_t pixelCount, size_t maxColors, uint32_t* outPalette);
