#pragma once

#include "common.h"
#include <stdint.h>
#include <stddef.h>

// 32-bit ARGB pixel: (a << 24) | (r << 16) | (g << 8) | b. Owns the pixels array.
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t* pixels;
} PixelImage;

// Result of cropping transparent borders from an image. The cropped image owns its pixels.
typedef struct {
    PixelImage image;
    int32_t offsetX;
    int32_t offsetY;
} CropResult;

PixelImage PixelImage_create(uint32_t width, uint32_t height);
void PixelImage_free(PixelImage* image);

// Decodes a GameMaker texture blob (PNG/QOI/BZ2-QOI) into an ARGB PixelImage. gm2022_5 should be set when the data.win is GMS 2022.5 or later (different BZ2-QOI header).
// Returns a PixelImage with width=0,height=0,pixels=nullptr on failure.
PixelImage PixelImage_decode(const uint8_t* blob, size_t blobSize, bool gm2022_5);

// Extracts a sub-rectangle from an image into a new PixelImage. The returned image owns its pixels.
PixelImage PixelImage_extractSubImage(const PixelImage* src, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height);

// Extracts a TPAG region from a texture page, padded to the bounding rectangle (Kotlin extractFromTPAG semantics).
PixelImage PixelImage_extractFromTPAG(const PixelImage* texPage,
    uint16_t sourceX, uint16_t sourceY,
    uint16_t targetX, uint16_t targetY, uint16_t targetWidth, uint16_t targetHeight,
    uint16_t boundingWidth, uint16_t boundingHeight);

// Crops the all-transparent rows/columns from the borders of an image. The original image is not modified. Fully transparent images return a 1x1 transparent image.
CropResult PixelImage_cropTransparentBorders(const PixelImage* image);

// Resizes an image using nearest-neighbor sampling. The returned image owns its pixels.
PixelImage PixelImage_resizeNearest(const PixelImage* src, uint32_t newWidth, uint32_t newHeight);
