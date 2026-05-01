#pragma once

#include "common.h"
#include "pixel_image.h"
#include <stdint.h>
#include <stddef.h>

// A palette-indexed image for PS2 (PSMT4 = 4bpp/16 colors, PSMT8 = 8bpp/256 colors)
typedef struct {
    char* name;            // owned strdup of the image name
    uint32_t width;
    uint32_t height;
    uint8_t bpp;           // 4 or 8
    uint32_t* palette;     // ARGB colors, sorted by unsigned value, padded to 16 or 256
    uint32_t paletteSlots; // 16 or 256 (size of palette array)
    uint32_t usedColors;   // actual number of used palette entries
    uint8_t* indices;      // one byte per pixel (palette index), size = width*height (owned)
    bool ownsPalette;      // true while the palette is unique to this image; false after dedup/merge points it at a ClutGroup-owned palette
} ClutImage;

// A shared CLUT group (multiple images can reference the same CLUT)
typedef struct {
    int32_t id;
    uint8_t bpp;
    uint32_t* colors;          // sorted (unsigned) used colors, length = colorCount (owned)
    uint32_t colorCount;
    uint32_t* palette;         // padded palette (16 or 256 slots) (owned)
    int32_t* imageIndices;     // indices into the global images[] array (owned)
    uint32_t imageIndexCount;
    uint32_t imageIndexCapacity;
} ClutGroup;

// Builds a ClutImage from a PixelImage. force4bpp: if true, even >16-color images get quantized to 4bpp instead of 8bpp.
ClutImage ClutProcessor_createClutImage(const char* name, const PixelImage* image, bool force4bpp);

// Frees an owned ClutImage. Skips palette free when ownsPalette is false (palette is owned by a ClutGroup).
void ClutImage_free(ClutImage* image);

// Frees a ClutGroup (palette, colors, imageIndices array).
void ClutGroup_free(ClutGroup* group);

// Deduplicate CLUTs: group images with identical palette color sets. Returns a freshly allocated array of ClutGroups (owned by caller).
// Each input ClutImage's palette pointer is updated to share the group's palette and ownsPalette is cleared.
ClutGroup* ClutProcessor_deduplicateCluts(ClutImage* images, size_t imageCount, size_t* outGroupCount);

// Merge CLUTs that have available slots with compatible CLUTs (smaller groups first). Returns a NEW array of ClutGroups; the input groups are freed.
// Each ClutImage's palette pointer and indices are updated to point at the merged palette.
ClutGroup* ClutProcessor_mergeCluts(ClutImage* images, size_t imageCount, ClutGroup* initialGroups, size_t initialGroupCount, size_t* outGroupCount);

// Get the set of actually-used colors from a ClutImage's palette (for use building a ClutGroup).
// Writes the colors to outColors (which must have capacity >= image->usedColors). Returns number written.
uint32_t ClutImage_getUsedColors(const ClutImage* image, uint32_t* outColors);
