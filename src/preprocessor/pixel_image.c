#include "pixel_image.h"
#include "image_decoder.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

PixelImage PixelImage_create(uint32_t width, uint32_t height) {
    PixelImage img;
    img.width = width;
    img.height = height;
    if (width == 0 || height == 0) {
        img.pixels = nullptr;
    } else {
        img.pixels = safeCalloc((size_t) width * height, sizeof(uint32_t));
    }
    return img;
}

void PixelImage_free(PixelImage* image) {
    free(image->pixels);
    image->pixels = nullptr;
    image->width = 0;
    image->height = 0;
}

PixelImage PixelImage_decode(const uint8_t* blob, size_t blobSize, bool gm2022_5) {
    int w = 0, h = 0;
    uint8_t* rgba = ImageDecoder_decodeToRgba(blob, blobSize, gm2022_5, &w, &h);
    if (rgba == nullptr) return (PixelImage){.width = 0, .height = 0, .pixels = nullptr};

    PixelImage img = PixelImage_create((uint32_t) w, (uint32_t) h);
    size_t pixelCount = (size_t) w * (size_t) h;
    repeat(pixelCount, i) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        img.pixels[i] = ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
    }
    free(rgba);
    return img;
}

PixelImage PixelImage_extractSubImage(const PixelImage* src, uint32_t srcX, uint32_t srcY, uint32_t width, uint32_t height) {
    PixelImage out = PixelImage_create(width, height);
    repeat(height, y) {
        repeat(width, x) {
            out.pixels[y * width + x] = src->pixels[(srcY + y) * src->width + (srcX + x)];
        }
    }
    return out;
}

PixelImage PixelImage_extractFromTPAG(const PixelImage* texPage,
    uint16_t sourceX, uint16_t sourceY,
    uint16_t targetX, uint16_t targetY, uint16_t targetWidth, uint16_t targetHeight,
    uint16_t boundingWidth, uint16_t boundingHeight)
{
    uint32_t w = boundingWidth > 0 ? boundingWidth : 1;
    uint32_t h = boundingHeight > 0 ? boundingHeight : 1;
    PixelImage img = PixelImage_create(w, h);

    if (texPage == nullptr || texPage->pixels == nullptr) return img;
    if (targetWidth == 0 || targetHeight == 0) return img;

    repeat(targetHeight, dy) {
        uint32_t srcRowY = sourceY + dy;
        uint32_t dstRowY = targetY + dy;
        if (srcRowY >= texPage->height || dstRowY >= h) continue;
        repeat(targetWidth, dx) {
            uint32_t srcColX = sourceX + dx;
            uint32_t dstColX = targetX + dx;
            if (srcColX >= texPage->width || dstColX >= w) continue;
            img.pixels[dstRowY * w + dstColX] = texPage->pixels[srcRowY * texPage->width + srcColX];
        }
    }
    return img;
}

CropResult PixelImage_cropTransparentBorders(const PixelImage* image) {
    int32_t minY = -1;
    repeat(image->height, y) {
        repeat(image->width, x) {
            if ((image->pixels[y * image->width + x] >> 24) != 0) {
                minY = (int32_t) y;
                break;
            }
        }
        if (minY >= 0) break;
    }

    if (0 > minY) {
        // Fully transparent: return a 1x1 transparent image
        PixelImage img = PixelImage_create(1, 1);
        return (CropResult){.image = img, .offsetX = 0, .offsetY = 0};
    }

    int32_t maxY = minY;
    for (int32_t y = (int32_t) image->height - 1; y >= minY; y--) {
        bool found = false;
        repeat(image->width, x) {
            if ((image->pixels[y * image->width + x] >> 24) != 0) {
                maxY = y;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    int32_t minX = (int32_t) image->width;
    for (int32_t y = minY; y <= maxY; y++) {
        for (int32_t x = 0; x < minX; x++) {
            if ((image->pixels[y * image->width + x] >> 24) != 0) {
                minX = x;
                break;
            }
        }
    }

    int32_t maxX = minX;
    for (int32_t y = minY; y <= maxY; y++) {
        for (int32_t x = (int32_t) image->width - 1; x > maxX; x--) {
            if ((image->pixels[y * image->width + x] >> 24) != 0) {
                maxX = x;
                break;
            }
        }
    }

    uint32_t cropW = (uint32_t)(maxX - minX + 1);
    uint32_t cropH = (uint32_t)(maxY - minY + 1);
    PixelImage img = PixelImage_create(cropW, cropH);
    repeat(cropH, y) {
        repeat(cropW, x) {
            img.pixels[y * cropW + x] = image->pixels[(minY + y) * image->width + (minX + x)];
        }
    }
    return (CropResult){.image = img, .offsetX = minX, .offsetY = minY};
}

PixelImage PixelImage_resizeNearest(const PixelImage* src, uint32_t newWidth, uint32_t newHeight) {
    PixelImage out = PixelImage_create(newWidth, newHeight);
    repeat(newHeight, y) {
        uint32_t srcY = (uint32_t)((y * src->height) / newHeight);
        repeat(newWidth, x) {
            uint32_t srcX = (uint32_t)((x * src->width) / newWidth);
            out.pixels[y * newWidth + x] = src->pixels[srcY * src->width + srcX];
        }
    }
    return out;
}
