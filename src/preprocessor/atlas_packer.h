#pragma once

#include "common.h"
#include "clut_processor.h"
#include <stdint.h>
#include <stddef.h>

#define ATLAS_PACKER_MAX_SIZE 512

typedef struct {
    int32_t imageIndex; // index into the images[] array passed to packAtlases
    uint32_t x;
    uint32_t y;
} AtlasEntry;

typedef struct {
    int32_t id;
    uint8_t bpp;
    uint32_t width;
    uint32_t height;
    AtlasEntry* entries;
    uint32_t entryCount;
    uint32_t entryCapacity;
} TextureAtlas;

// Each (image-name, group-key) pair tells the packer which images to keep together in the same atlas
typedef struct {
    const char* imageName; // not owned; must outlive the call
    const char* groupKey;  // not owned
} AtlasGroupEntry;

void TextureAtlas_free(TextureAtlas* atlas);

// Pack ClutImages into texture atlases, grouped by groupKey and separated by bpp. Returns a freshly allocated TextureAtlas array (caller frees each entry + the array).
TextureAtlas* TextureAtlasPacker_packAtlases(ClutImage* images, size_t imageCount, AtlasGroupEntry* groupEntries, size_t groupEntryCount, size_t* outAtlasCount);
