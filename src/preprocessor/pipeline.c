#include "pipeline.h"
#include "atlas_packer.h"
#include "audio_codec.h"
#include "byte_writer.h"
#include "clut_processor.h"
#include "data_win.h"
#include "image_decoder.h"
#include "pixel_image.h"
#include "utils.h"

#include "stb_ds.h"

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ Atlas group resolution ]===

// Omega Flowey sprite prefixes that should all be packed into the same atlas to reduce VRAM evictions during the fight
static const char* OMEGA_FLOWEY_PREFIXES[] = {
    "spr_floweyx_tv",
    "spr_tvinside",
    "spr_lefteye_",
    "spr_flipeye_",
    "spr_floweyx_mouth",
    "spr_floweyx_dimple",
    "spr_mouthball",
    "spr_mouthbeam",
    "spr_mouthflash",
    "spr_floweyx_fleshmound",
    "spr_fleshmound",
    "spr_nostrils",
    "spr_dentata_",
    "spr_halfdentata_",
    "spr_floweyx_flame",
    "spr_floweynuke",
    "spr_venus_placeholder",
    "spr_bgpipe",
    "spr_pipepart",
    "spr_sidestalk",
    "spr_vines_flowey",
    "spr_floweyarm",
    "spr_tv_warning",
    "spr_tv_exface",
    "spr_tv_floweyface",
    "spr_tv_floweyx_",
    "spr_noise",
};
static const size_t OMEGA_FLOWEY_PREFIX_COUNT = sizeof(OMEGA_FLOWEY_PREFIXES) / sizeof(OMEGA_FLOWEY_PREFIXES[0]);

#define OMEGA_FLOWEY_GROUP "spr/_omega_flowey"

static const char* getAtlasGroupKey(const char* spriteName, char* fallbackBuffer, size_t fallbackSize) {
    repeat(OMEGA_FLOWEY_PREFIX_COUNT, i) {
        const char* prefix = OMEGA_FLOWEY_PREFIXES[i];
        if (strncmp(spriteName, prefix, strlen(prefix)) == 0) return OMEGA_FLOWEY_GROUP;
    }
    snprintf(fallbackBuffer, fallbackSize, "spr/%s", spriteName);
    return fallbackBuffer;
}

// ===[ Resize-overrides for problematic sprites ]===

// Resize any images exceeding 512x512.
// We'll also resize any "problematic" sprites
static uint32_t getMaxSpriteDim(const char* name) {
    if (strncmp(name, "spr/spr_sidestalk", 17) == 0) return 16;
    if (strncmp(name, "spr/spr_mouthball_", 18) == 0) return 32;
    if (strncmp(name, "spr/spr_fa_seq_b_", 17) == 0) return 64;
    if (strncmp(name, "spr/spr_floweyarm", 17) == 0) return 64;
    if (strncmp(name, "spr/spr_fa_stemunder_", 21) == 0) return 64;
    if (strncmp(name, "spr/spr_floweynuke_explosion", 28) == 0) return 16;
    if (strncmp(name, "spr/spr_floweyx_flame_move_", 27) == 0) return 16;
    if (strncmp(name, "spr/spr_venus_placeholder", 25) == 0) return 32;
    if (strncmp(name, "spr/spr_f_handgun_neo_", 22) == 0) return 32;
    if (strncmp(name, "spr/spr_vines_flowey", 20) == 0) return 32;
    if (strncmp(name, "spr/spr_floweyx_flamethrower", 28) == 0) return 64;
    if (strncmp(name, "spr/spr_floweynuke", 18) == 0) return 32;
    if (strncmp(name, "spr/spr_bgpipe", 14) == 0) return 64;
    if (strncmp(name, "spr/spr_tvinside_old", 20) == 0) return 64;
    if (strncmp(name, "spr/spr_floweyx_tv", 18) == 0) return 128;
    if (strncmp(name, "spr/spr_floweyx_fleshmound", 26) == 0) return 64;
    if (strncmp(name, "spr/spr_tv_floweyx_laugh", 24) == 0) return 64;
    if (strncmp(name, "spr/spr_floweyx_mouthedge", 25) == 0) return 64;
    return 512;
}

// ===[ Image collection ]===

typedef struct {
    char* name;        // owned
    PixelImage image;  // owned
    int32_t tpagIndex; // -1 if not from a TPAG (i.e. it's a tile sub-rect)
} CollectedImage;

typedef struct {
    bool useSpriteDefinition;
    int32_t bgDef;
    int32_t srcX;
    int32_t srcY;
    uint32_t w;
    uint32_t h;
} TileKey;

typedef struct {
    TileKey key;
    int32_t imageIndex; // index into images[] once collected
} UniqueTile;

typedef struct {
    int32_t offsetX;
    int32_t offsetY;
    uint32_t croppedWidth;
    uint32_t croppedHeight;
} CropInfo;

// ===[ Audio writer helpers ]===

// Write SOUNDBNK.BIN header + SOND entries + AUDO entries + MUS string table + MUS entries.
static uint8_t* writeSoundBnkBytes(
    DataWin* dw,
    AudioData* parsedAudio, size_t parsedAudioCount,
    const int32_t* sondIdxToAudoIndex, // -1 if no override
    AudioData* musAudio, char** musPaths, size_t musCount,
    const uint32_t* audioOffsets, const uint32_t* audioSizes,
    const uint32_t* musOffsets, const uint32_t* musSizes,
    size_t* outSize)
{
    ByteWriter w = ByteWriter_create(1024);
    // Header (7 bytes)
    ByteWriter_writeUint8(&w, 0);                            // version
    ByteWriter_writeUint16(&w, (uint16_t) dw->sond.count);   // sondEntryCount
    ByteWriter_writeUint16(&w, (uint16_t) parsedAudioCount); // audoEntryCount
    ByteWriter_writeUint16(&w, (uint16_t) musCount);         // musEntryCount

    // SOND entries (12 bytes each)
    repeat(dw->sond.count, sondIdx) {
        Sound* sound = &dw->sond.sounds[sondIdx];
        // Use the external mapping if available, otherwise use the original audioFile index
        int32_t audoIndex = sondIdxToAudoIndex[sondIdx];
        if (0 > audoIndex) {
            if (sound->audioFile < 0 || sound->audioFile >= (int32_t) parsedAudioCount) audoIndex = 0xFFFF;
            else audoIndex = sound->audioFile;
        }
        ByteWriter_writeUint16(&w, (uint16_t) audoIndex);                            // audoIndex
        ByteWriter_writeUint32(&w, sound->flags);                                    // flags
        ByteWriter_writeUint16(&w, (uint16_t)((int)(sound->volume * 256.0f)));       // volume (fixed-point)
        ByteWriter_writeUint16(&w, (uint16_t)((int)(sound->pitch * 256.0f)));        // pitch (fixed-point)
        ByteWriter_writeUint16(&w, 0);                                               // reserved
    }

    // AUDO entries (20 bytes each)
    repeat(parsedAudioCount, i) {
        AudioData* audio = &parsedAudio[i];
        if (audio->valid) {
            ByteWriter_writeUint32(&w, audioOffsets[i]);                  // dataOffset
            ByteWriter_writeUint32(&w, audioSizes[i]);                    // dataSize
            ByteWriter_writeUint16(&w, (uint16_t) audio->sampleRate);     // sampleRate
            ByteWriter_writeUint8(&w, (uint8_t) audio->channels);         // channels
            ByteWriter_writeUint8(&w, (uint8_t) audio->bitsPerSample);    // bitsPerSample
            ByteWriter_writeUint8(&w, (uint8_t) audio->format);           // format (0=PCM, 1=ADPCM)
            ByteWriter_writeUint8(&w, 0);                                 // reserved
            ByteWriter_writeUint16(&w, 0);                                // reserved
            ByteWriter_writeUint32(&w, (uint32_t) audio->sampleCount);    // sampleCount (per channel)
        } else {
            // Unmapped entry
            ByteWriter_writeUint32(&w, 0);
            ByteWriter_writeUint32(&w, 0);
            ByteWriter_writeUint16(&w, 0);
            ByteWriter_writeUint8(&w, 0);
            ByteWriter_writeUint8(&w, 0);
            ByteWriter_writeUint8(&w, 0);
            ByteWriter_writeUint8(&w, 0);
            ByteWriter_writeUint16(&w, 0);
            ByteWriter_writeUint32(&w, 0);
        }
    }

    // MUS string table (variable size: u8 nameLength + name bytes per entry)
    repeat(musCount, i) {
        size_t nameLen = strlen(musPaths[i]);
        ByteWriter_writeUint8(&w, (uint8_t) nameLen);
        ByteWriter_writeBytes(&w, (const uint8_t*) musPaths[i], nameLen);
    }

    // MUS entries (16 bytes each)
    repeat(musCount, i) {
        AudioData* audio = &musAudio[i];
        ByteWriter_writeUint32(&w, musOffsets[i]);                 // dataOffset in SOUNDS.BIN
        ByteWriter_writeUint32(&w, musSizes[i]);                   // dataSize
        ByteWriter_writeUint16(&w, (uint16_t) audio->sampleRate);  // sampleRate
        ByteWriter_writeUint8(&w, (uint8_t) audio->channels);      // channels
        ByteWriter_writeUint8(&w, (uint8_t) audio->format);        // format (0=PCM, 1=ADPCM)
        ByteWriter_writeUint32(&w, (uint32_t) audio->sampleCount); // sampleCount (per channel)
    }

    return ByteWriter_detach(&w, outSize);
}

// ===[ CLUT writers ]===

// Converts ARGB palette to PS2 RGBA format with alpha remapped to 0-128 range
static uint32_t convertARGBtoPS2RGBA(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFF;
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8) & 0xFF;
    uint32_t b = argb & 0xFF;
    // PS2 alpha: 0-255 -> 0-128 (>> 1) + 1, fully opaque = 0x80
    uint32_t ps2Alpha = (a + 1) >> 1;
    return (ps2Alpha << 24) | (b << 16) | (g << 8) | r;
}

// PS2 CSM1 CLUT swizzle for 8bpp
static void swizzlePalette8bpp(uint32_t* palette) {
    repeat(256, i) {
        if ((i & 0x18) == 8) {
            uint32_t tmp = palette[i];
            palette[i] = palette[i + 8];
            palette[i + 8] = tmp;
        }
    }
}

static int compareGroupsById(const void* a, const void* b) {
    const ClutGroup* ga = (const ClutGroup*) a;
    const ClutGroup* gb = (const ClutGroup*) b;
    return ga->id - gb->id;
}

static uint8_t* writeClutBinary(ClutGroup* mergedGroups, size_t mergedCount, uint8_t targetBpp, uint32_t paletteSize, size_t* outSize) {
    // Filter and sort by id
    ClutGroup* filtered = safeMalloc(mergedCount * sizeof(ClutGroup));
    size_t filteredCount = 0;
    repeat(mergedCount, i) {
        if (mergedGroups[i].bpp == targetBpp) filtered[filteredCount++] = mergedGroups[i];
    }
    qsort(filtered, filteredCount, sizeof(ClutGroup), compareGroupsById);

    if (filteredCount == 0) {
        free(filtered);
        *outSize = 0;
        return nullptr;
    }

    ByteWriter w = ByteWriter_create(filteredCount * paletteSize * 4);
    repeat(filteredCount, gi) {
        ClutGroup* group = &filtered[gi];
        uint32_t* palette = safeCalloc(paletteSize, sizeof(uint32_t));
        uint32_t copyCount = group->colorCount < paletteSize ? group->colorCount : paletteSize;
        repeat(copyCount, i) palette[i] = group->palette[i];

        // Convert ARGB to PS2 RGBA with alpha remap
        repeat(paletteSize, i) palette[i] = convertARGBtoPS2RGBA(palette[i]);

        // Swizzle 8bpp palettes for CSM1
        if (paletteSize == 256) swizzlePalette8bpp(palette);

        repeat(paletteSize, i) ByteWriter_writeUint32(&w, palette[i]);
        free(palette);
    }
    free(filtered);
    return ByteWriter_detach(&w, outSize);
}

// ===[ Texture page (atlas pixel data) writer ]===

static uint8_t* rleCompress(const uint8_t* data, size_t len, size_t* outSize) {
    if (len == 0) {
        *outSize = 0;
        return nullptr;
    }
    ByteWriter w = ByteWriter_create(len);
    size_t i = 0;
    while (len > i) {
        uint8_t current = data[i];
        size_t runLength = 1;
        while (len > i + runLength && runLength < 255 && data[i + runLength] == current) {
            runLength++;
        }
        ByteWriter_writeUint8(&w, (uint8_t) runLength);
        ByteWriter_writeUint8(&w, current);
        i += runLength;
    }
    return ByteWriter_detach(&w, outSize);
}

static int compareAtlasById(const void* a, const void* b) {
    const TextureAtlas* aa = (const TextureAtlas*) a;
    const TextureAtlas* ab = (const TextureAtlas*) b;
    return aa->id - ab->id;
}

static uint8_t* writeTexturePagesBytes(TextureAtlas* atlases, size_t atlasCount, ClutImage* images, uint64_t** outAtlasOffsetsByAtlasId, size_t* outSize) {
    const uint32_t headerSize = 128;
    uint64_t currentOffset = 0;

    TextureAtlas* sortedAtlases = safeMalloc(atlasCount * sizeof(TextureAtlas));
    memcpy(sortedAtlases, atlases, atlasCount * sizeof(TextureAtlas));
    qsort(sortedAtlases, atlasCount, sizeof(TextureAtlas), compareAtlasById);

    uint64_t* atlasOffsets = safeCalloc(atlasCount, sizeof(uint64_t));

    ByteWriter w = ByteWriter_create(1024 * 1024);
    repeat(atlasCount, ai) {
        TextureAtlas* atlas = &sortedAtlases[ai];
        atlasOffsets[atlas->id] = currentOffset;

        // Composite all entries into a single pixel index buffer
        uint8_t* canvas = safeCalloc((size_t) atlas->width * atlas->height, 1);
        repeat(atlas->entryCount, ei) {
            AtlasEntry* entry = &atlas->entries[ei];
            ClutImage* img = &images[entry->imageIndex];
            repeat(img->height, y) {
                repeat(img->width, x) {
                    canvas[(entry->y + y) * atlas->width + (entry->x + x)] = img->indices[y * img->width + x];
                }
            }
        }

        // Pack pixel data according to bpp
        uint8_t* uncompressedPixelData;
        size_t uncompressedSize;
        if (atlas->bpp == 4) {
            size_t packedSize = ((size_t) atlas->width * atlas->height + 1) / 2;
            uncompressedPixelData = safeMalloc(packedSize);
            uncompressedSize = packedSize;
            size_t canvasSize = (size_t) atlas->width * atlas->height;
            for (size_t i = 0; i < canvasSize; i += 2) {
                uint8_t lo = canvas[i] & 0x0F;
                uint8_t hi = (canvasSize > i + 1) ? ((canvas[i + 1] & 0x0F) << 4) : 0;
                uncompressedPixelData[i / 2] = lo | hi;
            }
            free(canvas);
        } else {
            uncompressedPixelData = canvas;
            uncompressedSize = (size_t) atlas->width * atlas->height;
        }

        // Try RLE compression
        size_t rleSize;
        uint8_t* rleData = rleCompress(uncompressedPixelData, uncompressedSize, &rleSize);
        bool useRle = uncompressedSize > rleSize;
        uint8_t compressionType;
        const uint8_t* pixelData;
        size_t pixelDataSize;
        if (useRle) {
            compressionType = 1;
            pixelData = rleData;
            pixelDataSize = rleSize;
            printf("  Atlas %d (%dbpp): RLE compressed %zu -> %zu bytes (saved %zu bytes, %d%%)\n",
                atlas->id, (int) atlas->bpp, uncompressedSize, rleSize, uncompressedSize - rleSize,
                (int)((100.0 * (double)(uncompressedSize - rleSize)) / (double) uncompressedSize));
        } else {
            compressionType = 0;
            pixelData = uncompressedPixelData;
            pixelDataSize = uncompressedSize;
            printf("  Atlas %d (%dbpp): RLE not beneficial (%zu -> %zu bytes), keeping uncompressed\n",
                atlas->id, (int) atlas->bpp, uncompressedSize, rleSize);
        }

        // Header (128 bytes)
        ByteWriter_writeUint8(&w, 0);                                  // version
        ByteWriter_writeUint16(&w, (uint16_t) atlas->width);           // width
        ByteWriter_writeUint16(&w, (uint16_t) atlas->height);          // height
        ByteWriter_writeUint8(&w, atlas->bpp);                         // bpp
        ByteWriter_writeUint32(&w, (uint32_t) pixelDataSize);          // pixelDataSize
        ByteWriter_writeUint8(&w, compressionType);                    // compressionType (0 = uncompressed, 1 = RLE)
        // Pad to 128 bytes (we wrote 1+2+2+1+4+1 = 11 bytes so far)
        ByteWriter_writeZeroPadding(&w, headerSize - 11);

        // Pixel data
        ByteWriter_writeBytes(&w, pixelData, pixelDataSize);

        free(rleData);
        free(uncompressedPixelData);
        currentOffset += headerSize + pixelDataSize;
    }
    free(sortedAtlases);
    *outAtlasOffsetsByAtlasId = atlasOffsets;
    return ByteWriter_detach(&w, outSize);
}

// ===[ ATLAS.BIN writer ]===

typedef struct {
    int32_t atlasId;
    uint32_t atlasX;
    uint32_t atlasY;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
} AtlasEntryInfo;

static uint8_t* writeAtlasMetadataBytes(
    DataWin* dw,
    UniqueTile* uniqueTiles, size_t uniqueTileCount,
    int32_t* tpagIdxToImageIndex, // size = dw->tpag.count
    AtlasEntryInfo* atlasEntryByImage, // size = imageCount
    int32_t* clutIndexByImage, // size = imageCount
    const CropInfo* cropByImage, // size = imageCount
    const uint64_t* atlasOffsets, size_t atlasCount, ClutImage* images,
    size_t* outSize)
{
    uint32_t tpagCount = dw->tpag.count;
    ByteWriter w = ByteWriter_create(4096);

    // Header
    ByteWriter_writeUint8(&w, 0);                                  // version
    ByteWriter_writeUint16(&w, (uint16_t) tpagCount);              // tpagEntryCount
    ByteWriter_writeUint16(&w, (uint16_t) uniqueTileCount);        // tileEntryCount
    ByteWriter_writeUint16(&w, (uint16_t) atlasCount);             // atlasCount

    // Atlas offset table (sorted by id, which is the array index since ids are 0..atlasCount-1 and contiguous)
    repeat(atlasCount, id) ByteWriter_writeUint32(&w, (uint32_t) atlasOffsets[id]);

    // TPAG entries
    repeat(tpagCount, tpagIdx) {
        int32_t imgIdx = tpagIdxToImageIndex[tpagIdx];
        if (imgIdx >= 0 && atlasEntryByImage[imgIdx].atlasId >= 0) {
            AtlasEntryInfo* e = &atlasEntryByImage[imgIdx];
            ByteWriter_writeUint16(&w, (uint16_t) e->atlasId);
            ByteWriter_writeUint16(&w, (uint16_t) e->atlasX);
            ByteWriter_writeUint16(&w, (uint16_t) e->atlasY);
            ByteWriter_writeUint16(&w, (uint16_t) e->width);
            ByteWriter_writeUint16(&w, (uint16_t) e->height);
            ByteWriter_writeUint16(&w, (uint16_t) cropByImage[imgIdx].offsetX);
            ByteWriter_writeUint16(&w, (uint16_t) cropByImage[imgIdx].offsetY);
            ByteWriter_writeUint16(&w, (uint16_t) cropByImage[imgIdx].croppedWidth);
            ByteWriter_writeUint16(&w, (uint16_t) cropByImage[imgIdx].croppedHeight);
            ByteWriter_writeUint16(&w, (uint16_t) clutIndexByImage[imgIdx]);
            ByteWriter_writeUint8(&w, e->bpp);
            continue;
        }
        // Unmapped TPAG entry
        ByteWriter_writeUint16(&w, 0xFFFF); // atlasId = unmapped
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint16(&w, 0);
        ByteWriter_writeUint8(&w, 0);
    }

    // Tile entries
    repeat(uniqueTileCount, ti) {
        UniqueTile* tile = &uniqueTiles[ti];
        int32_t imgIdx = tile->imageIndex;
        AtlasEntryInfo* e = (imgIdx >= 0) ? &atlasEntryByImage[imgIdx] : nullptr;
        ClutImage* img = (imgIdx >= 0) ? &images[imgIdx] : nullptr;

        ByteWriter_writeUint16(&w, (uint16_t) tile->key.bgDef);
        ByteWriter_writeUint16(&w, (uint16_t) tile->key.srcX);
        ByteWriter_writeUint16(&w, (uint16_t) tile->key.srcY);
        ByteWriter_writeUint16(&w, (uint16_t) tile->key.w);
        ByteWriter_writeUint16(&w, (uint16_t) tile->key.h);
        ByteWriter_writeUint16(&w, (uint16_t)((e && e->atlasId >= 0) ? e->atlasId : 0xFFFF));
        ByteWriter_writeUint16(&w, (uint16_t)(e ? e->atlasX : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(e ? e->atlasY : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(img ? img->width : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(img ? img->height : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(imgIdx >= 0 ? cropByImage[imgIdx].offsetX : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(imgIdx >= 0 ? cropByImage[imgIdx].offsetY : 0));
        ByteWriter_writeUint16(&w, (uint16_t)(imgIdx >= 0 ? cropByImage[imgIdx].croppedWidth : (img ? img->width : 0)));
        ByteWriter_writeUint16(&w, (uint16_t)(imgIdx >= 0 ? cropByImage[imgIdx].croppedHeight : (img ? img->height : 0)));
        ByteWriter_writeUint16(&w, (uint16_t)(imgIdx >= 0 ? clutIndexByImage[imgIdx] : 0));
        ByteWriter_writeUint8(&w, (uint8_t)(e ? e->bpp : 0));
    }

    return ByteWriter_detach(&w, outSize);
}

// ===[ Tile collection helpers ]===

static bool tileKeyEquals(const TileKey* a, const TileKey* b) {
    return a->useSpriteDefinition == b->useSpriteDefinition &&
           a->bgDef == b->bgDef &&
           a->srcX == b->srcX &&
           a->srcY == b->srcY &&
           a->w == b->w &&
           a->h == b->h;
}

static int collectTile(UniqueTile** tiles, RoomTile* tile, DataWin* dw) {
    int32_t defCount = (int32_t)(tile->useSpriteDefinition ? dw->sprt.count : dw->bgnd.count);
    if (0 > tile->backgroundDefinition || tile->backgroundDefinition >= defCount) return -1;

    TileKey key = {.useSpriteDefinition = tile->useSpriteDefinition, .bgDef = tile->backgroundDefinition,
                   .srcX = tile->sourceX, .srcY = tile->sourceY, .w = tile->width, .h = tile->height};

    repeat(arrlen(*tiles), i) {
        if (tileKeyEquals(&(*tiles)[i].key, &key)) return -1;
    }
    UniqueTile entry = {.key = key, .imageIndex = -1};
    arrput(*tiles, entry);
    return (int) (arrlen(*tiles) - 1);
}

// ===[ Pipeline orchestrator ]===

void ProcessingResult_free(ProcessingResult* result) {
    free(result->gameName);
    free(result->clut4Bin);
    free(result->clut8Bin);
    free(result->texturesBin);
    free(result->atlasBin);
    free(result->soundBnkBin);
    free(result->soundsBin);
    memset(result, 0, sizeof(*result));
}

ProcessingResult Pipeline_processDataWin(const ProcessingOptions* options) {
    printf("Parsing data.win...\n");
    DataWinParserOptions parseOpts = {
        .parseGen8 = true, .parseSond = true, .parseSprt = true, .parseBgnd = true,
        .parseFont = true, .parseRoom = true, .parseTpag = true, .parseStrg = true,
        .parseTxtr = true, .parseAudo = true,
        .skipLoadingPreciseMasksForNonPreciseSprites = true,
    };
    DataWin* dw = DataWin_parse(options->dataWinPath, parseOpts);
    requireMessage(dw != nullptr, "DataWin_parse failed");

    printf("Parsed: %u sprites, %u backgrounds, %u fonts, %u textures, %u TPAG items\n",
        dw->sprt.count, dw->bgnd.count, dw->font.count, dw->txtr.count, dw->tpag.count);

    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);

    // Decode texture pages as PixelImages
    printf("Loading texture pages...\n");
    PixelImage* texturePages = safeCalloc(dw->txtr.count == 0 ? 1 : dw->txtr.count, sizeof(PixelImage));
    uint32_t loadedTexturePages = 0;
    repeat(dw->txtr.count, i) {
        Texture* tex = &dw->txtr.textures[i];
        if (tex->blobData != nullptr) {
            texturePages[i] = PixelImage_decode(tex->blobData, tex->blobSize, gm2022_5);
            if (texturePages[i].pixels != nullptr) loadedTexturePages++;
        }
    }
    printf("Loaded %u texture pages\n", loadedTexturePages);

    // Collect images: sprites, backgrounds, fonts, tiles
    CollectedImage* collected = nullptr; // stb_ds dynarray
    struct { char* key; char* value; }* atlasGroupKeyByName = nullptr; // owned strdup keys/values
    struct { char* key; int32_t value; }* tpagIndexByImageName = nullptr; // tpagIdx by image name (used for output)

    char fallbackBuf[256];

    // Helper: append to collected and bookkeeping maps
    #define APPEND_IMAGE(NAME, IMG, GROUPKEY, TPAGIDX) do {                                  \
        char* nameDup = safeStrdup(NAME);                                                    \
        CollectedImage ci = {.name = nameDup, .image = (IMG), .tpagIndex = (TPAGIDX)};       \
        arrput(collected, ci);                                                               \
        shput(atlasGroupKeyByName, nameDup, safeStrdup(GROUPKEY));                           \
        if ((TPAGIDX) >= 0) shput(tpagIndexByImageName, nameDup, (TPAGIDX));                 \
    } while (0)

    // Collect sprites
    repeat(dw->sprt.count, sprIdx) {
        Sprite* sprite = &dw->sprt.sprites[sprIdx];
        char nameBuf[256];
        const char* name = sprite->name ? sprite->name : (snprintf(nameBuf, sizeof(nameBuf), "sprite_%zu", sprIdx), nameBuf);
        const char* groupKey = getAtlasGroupKey(name, fallbackBuf, sizeof(fallbackBuf));
        char groupKeyOwned[256];
        snprintf(groupKeyOwned, sizeof(groupKeyOwned), "%s", groupKey);
        repeat(sprite->textureCount, frameIdx) {
            int32_t tpagIdx = sprite->tpagIndices[frameIdx];
            if (0 > tpagIdx) continue;
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            PixelImage* texPage = (tpag->texturePageId >= 0) ? &texturePages[tpag->texturePageId] : nullptr;
            PixelImage img = PixelImage_extractFromTPAG(texPage,
                tpag->sourceX, tpag->sourceY,
                tpag->targetX, tpag->targetY, tpag->targetWidth, tpag->targetHeight,
                tpag->boundingWidth, tpag->boundingHeight);
            char imgName[256];
            if (sprite->textureCount > 1) {
                snprintf(imgName, sizeof(imgName), "spr/%s_%zu", name, frameIdx);
            } else {
                snprintf(imgName, sizeof(imgName), "spr/%s", name);
            }
            APPEND_IMAGE(imgName, img, groupKeyOwned, tpagIdx);
        }
    }

    // Collect backgrounds
    repeat(dw->bgnd.count, bgIdx) {
        Background* bg = &dw->bgnd.backgrounds[bgIdx];
        char nameBuf[256];
        const char* name = bg->name ? bg->name : (snprintf(nameBuf, sizeof(nameBuf), "bg_%zu", bgIdx), nameBuf);
        int32_t tpagIdx = bg->tpagIndex;
        if (0 > tpagIdx) continue;
        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        PixelImage* texPage = (tpag->texturePageId >= 0) ? &texturePages[tpag->texturePageId] : nullptr;
        PixelImage img = PixelImage_extractFromTPAG(texPage,
            tpag->sourceX, tpag->sourceY,
            tpag->targetX, tpag->targetY, tpag->targetWidth, tpag->targetHeight,
            tpag->boundingWidth, tpag->boundingHeight);
        char imgName[256];
        snprintf(imgName, sizeof(imgName), "bg/%s", name);
        APPEND_IMAGE(imgName, img, imgName, tpagIdx);
    }

    // Collect fonts
    repeat(dw->font.count, fontIdx) {
        Font* font = &dw->font.fonts[fontIdx];
        char nameBuf[256];
        const char* name = font->name ? font->name : (snprintf(nameBuf, sizeof(nameBuf), "font_%zu", fontIdx), nameBuf);
        int32_t tpagIdx = font->tpagIndex;
        if (0 > tpagIdx) continue;
        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        PixelImage* texPage = (tpag->texturePageId >= 0) ? &texturePages[tpag->texturePageId] : nullptr;
        PixelImage img = PixelImage_extractFromTPAG(texPage,
            tpag->sourceX, tpag->sourceY,
            tpag->targetX, tpag->targetY, tpag->targetWidth, tpag->targetHeight,
            tpag->boundingWidth, tpag->boundingHeight);
        char imgName[256];
        snprintf(imgName, sizeof(imgName), "font/%s", name);
        APPEND_IMAGE(imgName, img, imgName, tpagIdx);
    }

    // Collect unique tiles (from legacy tiles and GMS2 asset layers)
    UniqueTile* uniqueTiles = nullptr;
    repeat(dw->room.count, ri) {
        Room* room = &dw->room.rooms[ri];
        repeat(room->tileCount, ti) collectTile(&uniqueTiles, &room->tiles[ti], dw);
        repeat(room->layerCount, li) {
            RoomLayer* layer = &room->layers[li];
            RoomLayerAssetsData* assets = layer->assetsData;
            if (assets == nullptr) continue;
            repeat(assets->legacyTileCount, ti) collectTile(&uniqueTiles, &assets->legacyTiles[ti], dw);
        }
    }

    // Extract source images for tiles (background or sprite depending on useSpriteDefinition)
    typedef struct { uint64_t key; PixelImage value; } TileSourceEntry;
    TileSourceEntry* tileSourceImages = nullptr;
    repeat(arrlen(uniqueTiles), ti) {
        TileKey* key = &uniqueTiles[ti].key;
        uint64_t srcKey = ((uint64_t) (key->useSpriteDefinition ? 1 : 0) << 32) | (uint32_t) key->bgDef;
        if (0 <= hmgeti(tileSourceImages, srcKey)) continue;
        int32_t tpagIdx = -1;
        if (key->useSpriteDefinition) {
            Sprite* sprite = &dw->sprt.sprites[key->bgDef];
            if (sprite->textureCount == 0) continue;
            tpagIdx = sprite->tpagIndices[0];
        } else {
            tpagIdx = dw->bgnd.backgrounds[key->bgDef].tpagIndex;
        }
        if (0 > tpagIdx) continue;
        TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
        PixelImage* texPage = (tpag->texturePageId >= 0) ? &texturePages[tpag->texturePageId] : nullptr;
        PixelImage img = PixelImage_extractFromTPAG(texPage,
            tpag->sourceX, tpag->sourceY,
            tpag->targetX, tpag->targetY, tpag->targetWidth, tpag->targetHeight,
            tpag->boundingWidth, tpag->boundingHeight);
        hmput(tileSourceImages, srcKey, img);
    }

    repeat(arrlen(uniqueTiles), ti) {
        TileKey* key = &uniqueTiles[ti].key;
        uint64_t srcKey = ((uint64_t) (key->useSpriteDefinition ? 1 : 0) << 32) | (uint32_t) key->bgDef;
        ptrdiff_t mi = hmgeti(tileSourceImages, srcKey);
        if (0 > mi) continue;
        PixelImage* srcImg = &tileSourceImages[mi].value;
        if ((uint32_t)(key->srcX + key->w) > srcImg->width || (uint32_t)(key->srcY + key->h) > srcImg->height) continue;
        if (key->w == 0 || key->h == 0) continue;
        PixelImage tileImg = PixelImage_extractSubImage(srcImg, key->srcX, key->srcY, key->w, key->h);
        const char* defName;
        char defNameBuf[64];
        if (key->useSpriteDefinition) {
            defName = dw->sprt.sprites[key->bgDef].name;
            if (defName == nullptr) { snprintf(defNameBuf, sizeof(defNameBuf), "spr%d", key->bgDef); defName = defNameBuf; }
        } else {
            defName = dw->bgnd.backgrounds[key->bgDef].name;
            if (defName == nullptr) { snprintf(defNameBuf, sizeof(defNameBuf), "bg%d", key->bgDef); defName = defNameBuf; }
        }
        char imgName[256];
        snprintf(imgName, sizeof(imgName), "tile/%s_%d_%d_%ux%u", defName, key->srcX, key->srcY, key->w, key->h);
        char groupKey[256];
        snprintf(groupKey, sizeof(groupKey), "tile/%s", defName);
        uniqueTiles[ti].imageIndex = (int32_t) arrlen(collected);
        APPEND_IMAGE(imgName, tileImg, groupKey, -1);
    }

    repeat(hmlen(tileSourceImages), i) PixelImage_free(&tileSourceImages[i].value);
    hmfree(tileSourceImages);

    // Free texture pages
    repeat(dw->txtr.count, i) PixelImage_free(&texturePages[i]);
    free(texturePages);

    // Crop transparent borders before packing (sprites only)
    size_t imageCount = arrlen(collected);
    CropInfo* cropByImage = safeCalloc(imageCount, sizeof(CropInfo));
    int croppedCount = 0;
    repeat(imageCount, i) {
        if (strncmp(collected[i].name, "spr/", 4) == 0) {
            CropResult crop = PixelImage_cropTransparentBorders(&collected[i].image);
            cropByImage[i] = (CropInfo){.offsetX = crop.offsetX, .offsetY = crop.offsetY, .croppedWidth = crop.image.width, .croppedHeight = crop.image.height};
            if (crop.image.width != collected[i].image.width || crop.image.height != collected[i].image.height) {
                croppedCount++;
            }
            PixelImage_free(&collected[i].image);
            collected[i].image = crop.image;
        } else {
            // No crop: store original dimensions for correct cropW/cropH in ATLAS.BIN
            cropByImage[i] = (CropInfo){.offsetX = 0, .offsetY = 0, .croppedWidth = collected[i].image.width, .croppedHeight = collected[i].image.height};
        }
    }
    if (croppedCount > 0) printf("Cropped transparent borders from %d sprite images\n", croppedCount);

    int resizedCount = 0;
    repeat(imageCount, i) {
        const char* name = collected[i].name;
        uint32_t maxDim = getMaxSpriteDim(name);
        PixelImage* img = &collected[i].image;
        if (maxDim >= img->width && maxDim >= img->height) continue;
        double scaleW = (double) maxDim / (double) img->width;
        double scaleH = (double) maxDim / (double) img->height;
        double scale = scaleW < scaleH ? scaleW : scaleH;
        uint32_t newW = (uint32_t)((double) img->width * scale);
        uint32_t newH = (uint32_t)((double) img->height * scale);
        if (newW < 1) newW = 1;
        if (newH < 1) newH = 1;
        PixelImage resized = PixelImage_resizeNearest(img, newW, newH);
        PixelImage_free(img);
        collected[i].image = resized;
        resizedCount++;
    }
    if (resizedCount > 0) printf("Resized %d images to fit\n", resizedCount);

    printf("Total images to process: %zu\n", imageCount);

    // Compile force4bpp regex patterns
    regex_t* force4bppMatchers = nullptr;
    if (options->force4bppPatternCount > 0) {
        force4bppMatchers = safeCalloc(options->force4bppPatternCount, sizeof(regex_t));
        repeat(options->force4bppPatternCount, i) {
            int rc = regcomp(&force4bppMatchers[i], options->force4bppPatterns[i], REG_EXTENDED | REG_NOSUB);
            if (rc != 0) {
                fprintf(stderr, "Failed to compile regex: %s\n", options->force4bppPatterns[i]);
                exit(1);
            }
        }
    }

    // Step 1: Create CLUTs
    printf("Creating CLUTs...\n");
    ClutImage* clutImages = safeCalloc(imageCount == 0 ? 1 : imageCount, sizeof(ClutImage));
    int forced4bppCount = 0;
    repeat(imageCount, i) {
        bool force4bpp = false;
        repeat(options->force4bppPatternCount, j) {
            if (regexec(&force4bppMatchers[j], collected[i].name, 0, nullptr, 0) == 0) {
                force4bpp = true;
                break;
            }
        }
        if (force4bpp) forced4bppCount++;
        clutImages[i] = ClutProcessor_createClutImage(collected[i].name, &collected[i].image, force4bpp);
    }
    if (force4bppMatchers != nullptr) {
        repeat(options->force4bppPatternCount, i) regfree(&force4bppMatchers[i]);
        free(force4bppMatchers);
        printf("  Forced %d images to 4bpp via %zu pattern(s)\n", forced4bppCount, options->force4bppPatternCount);
    }

    int bpp4Count = 0, bpp8Count = 0;
    repeat(imageCount, i) {
        if (clutImages[i].bpp == 4) bpp4Count++; else bpp8Count++;
    }
    printf("  4bpp: %d images, 8bpp: %d images\n", bpp4Count, bpp8Count);

    // Free the original PixelImages now that we have CLUTs
    repeat(imageCount, i) PixelImage_free(&collected[i].image);

    // Step 2: Deduplicate CLUTs
    printf("Deduplicating CLUTs...\n");
    size_t dedupCount;
    ClutGroup* dedupGroups = ClutProcessor_deduplicateCluts(clutImages, imageCount, &dedupCount);
    printf("  After dedup: %zu unique CLUTs (from %zu images)\n", dedupCount, imageCount);

    // Step 3: Merge CLUTs
    printf("Merging CLUTs...\n");
    size_t mergedCount;
    ClutGroup* mergedGroups = ClutProcessor_mergeCluts(clutImages, imageCount, dedupGroups, dedupCount, &mergedCount);
    int merged4 = 0, merged8 = 0;
    repeat(mergedCount, i) {
        if (mergedGroups[i].bpp == 4) merged4++; else merged8++;
    }
    printf("  After merge: %d merged 4bpp CLUTs, %d merged 8bpp CLUTs (%zu total)\n", merged4, merged8, mergedCount);

    // Step 4: Pack into texture atlases
    printf("Packing texture atlases...\n");
    AtlasGroupEntry* groupEntries = safeMalloc(imageCount * sizeof(AtlasGroupEntry));
    repeat(imageCount, i) {
        ptrdiff_t gki = shgeti(atlasGroupKeyByName, collected[i].name);
        groupEntries[i] = (AtlasGroupEntry){.imageName = collected[i].name, .groupKey = (gki >= 0) ? atlasGroupKeyByName[gki].value : collected[i].name};
    }
    size_t atlasCount;
    TextureAtlas* atlases = TextureAtlasPacker_packAtlases(clutImages, imageCount, groupEntries, imageCount, &atlasCount);
    free(groupEntries);
    int atlas4 = 0, atlas8 = 0;
    repeat(atlasCount, i) {
        if (atlases[i].bpp == 4) atlas4++; else atlas8++;
    }
    printf("  %d 4bpp atlases, %d 8bpp atlases (%zu total)\n", atlas4, atlas8, atlasCount);

    // Step 5: Write CLUT binaries
    printf("Writing CLUT binaries...\n");
    size_t clut4Size, clut8Size;
    uint8_t* clut4Bin = writeClutBinary(mergedGroups, mergedCount, 4, 16, &clut4Size);
    uint8_t* clut8Bin = writeClutBinary(mergedGroups, mergedCount, 8, 256, &clut8Size);

    // Step 6: Write texture pages
    printf("Writing texture pages...\n");
    uint64_t* atlasOffsetsByAtlasId;
    size_t texturesSize;
    uint8_t* texturesBin = writeTexturePagesBytes(atlases, atlasCount, clutImages, &atlasOffsetsByAtlasId, &texturesSize);

    // Step 7: Build lookups and write ATLAS.BIN
    printf("Writing ATLAS.BIN...\n");
    int32_t* clutIndexByImage = safeCalloc(imageCount == 0 ? 1 : imageCount, sizeof(int32_t));
    {
        // Sort merged groups by id, then assign per-bpp running indices
        ClutGroup* sortedMerged = safeMalloc(mergedCount * sizeof(ClutGroup));
        memcpy(sortedMerged, mergedGroups, mergedCount * sizeof(ClutGroup));
        qsort(sortedMerged, mergedCount, sizeof(ClutGroup), compareGroupsById);
        int clut4Idx = 0, clut8Idx = 0;
        repeat(mergedCount, gi) {
            ClutGroup* group = &sortedMerged[gi];
            int32_t idx = (group->bpp == 4) ? clut4Idx++ : clut8Idx++;
            repeat(group->imageIndexCount, k) {
                clutIndexByImage[group->imageIndices[k]] = idx;
            }
        }
        free(sortedMerged);
    }

    AtlasEntryInfo* atlasEntryByImage = safeMalloc(imageCount * sizeof(AtlasEntryInfo));
    repeat(imageCount, i) atlasEntryByImage[i] = (AtlasEntryInfo){.atlasId = -1};
    repeat(atlasCount, ai) {
        TextureAtlas* atlas = &atlases[ai];
        repeat(atlas->entryCount, ei) {
            AtlasEntry* entry = &atlas->entries[ei];
            atlasEntryByImage[entry->imageIndex] = (AtlasEntryInfo){
                .atlasId = atlas->id, .atlasX = entry->x, .atlasY = entry->y,
                .width = clutImages[entry->imageIndex].width, .height = clutImages[entry->imageIndex].height,
                .bpp = atlas->bpp
            };
        }
    }

    int32_t* tpagIdxToImageIndex = safeMalloc(dw->tpag.count * sizeof(int32_t));
    repeat(dw->tpag.count, i) tpagIdxToImageIndex[i] = -1;
    repeat(imageCount, i) {
        if (collected[i].tpagIndex >= 0) {
            tpagIdxToImageIndex[collected[i].tpagIndex] = (int32_t) i;
        }
    }

    size_t atlasBinSize;
    uint8_t* atlasBin = writeAtlasMetadataBytes(dw, uniqueTiles, arrlen(uniqueTiles), tpagIdxToImageIndex, atlasEntryByImage, clutIndexByImage, cropByImage, atlasOffsetsByAtlasId, atlasCount, clutImages, &atlasBinSize);

    free(tpagIdxToImageIndex);
    free(atlasEntryByImage);
    free(atlasOffsetsByAtlasId);
    free(clutIndexByImage);

    // Step 8: Process sounds
    printf("Processing sounds...\n");

    // Parse audiogroup files for their AUDO chunks. ExternalFile.fileName is a full filesystem path; the basename is "audiogroup<N>.dat".
    size_t maxGroupId = 0;
    repeat(options->audioGroupFileCount, i) {
        const char* path = options->audioGroupFiles[i].fileName;
        const char* slash = strrchr(path, '/');
        const char* basename = slash ? slash + 1 : path;
        const char* numStart = basename + strlen("audiogroup");
        size_t groupId = (size_t) atoi(numStart);
        if (groupId > maxGroupId) maxGroupId = groupId;
    }
    DataWin** audioGroupDws = safeCalloc(maxGroupId + 1, sizeof(DataWin*));
    repeat(options->audioGroupFileCount, i) {
        const char* path = options->audioGroupFiles[i].fileName;
        const char* slash = strrchr(path, '/');
        const char* basename = slash ? slash + 1 : path;
        const char* numStart = basename + strlen("audiogroup");
        size_t groupId = (size_t) atoi(numStart);
        printf("Parsing audiogroup%zu.dat...\n", groupId);
        DataWinParserOptions agOpts = {.parseAudo = true, .skipLoadingPreciseMasksForNonPreciseSprites = true};
        DataWin* agDw = DataWin_parse(path, agOpts);
        audioGroupDws[groupId] = agDw;
        if (agDw != nullptr) printf("  audiogroup%zu.dat: %u audio entries\n", groupId, agDw->audo.count);
    }

    printf("Decoding embedded audio files...\n");
    AudioData* parsedAudio = nullptr; // dynarray
    int embeddedCount = 0;
    repeat(dw->audo.count, idx) {
        AudioEntry* entry = &dw->audo.entries[idx];
        if (entry->data == nullptr) {
            AudioData empty = {.valid = false};
            arrput(parsedAudio, empty);
            continue;
        }
        AudioData decoded = AudioCodec_decode(entry->data, entry->dataSize);
        printf("  Decoded embedded audio #%zu%s\n", idx, decoded.valid ? "" : " (FAILED)");
        if (decoded.valid) embeddedCount++;
        arrput(parsedAudio, decoded);
    }

    // Decode AUDO entries from audiogroup files and map them
    int32_t* sondIdxToAudoIndex = safeMalloc(dw->sond.count * sizeof(int32_t));
    repeat(dw->sond.count, i) sondIdxToAudoIndex[i] = -1;

    int audioGroupCount = 0;
    repeat(dw->sond.count, sondIdx) {
        Sound* sound = &dw->sond.sounds[sondIdx];
        if (sound->audioGroup == 0) continue;
        if ((size_t) sound->audioGroup > maxGroupId) continue;
        DataWin* agDw = audioGroupDws[sound->audioGroup];
        if (agDw == nullptr) continue;
        if (sound->audioFile < 0 || sound->audioFile >= (int32_t) agDw->audo.count) continue;
        AudioEntry* entry = &agDw->audo.entries[sound->audioFile];
        if (entry->data == nullptr) continue;
        const char* label = sound->file ? sound->file : (sound->name ? sound->name : "audiogroup");
        AudioData decoded = AudioCodec_decode(entry->data, entry->dataSize);
        printf("  Decoded %s%s\n", label, decoded.valid ? "" : " (FAILED)");
        if (decoded.valid) {
            sondIdxToAudoIndex[sondIdx] = (int32_t) arrlen(parsedAudio);
            arrput(parsedAudio, decoded);
            audioGroupCount++;
        }
    }

    int externalCount = 0;
    printf("Decoding non-embedded audio files...\n");
    repeat(dw->sond.count, sondIdx) {
        Sound* sound = &dw->sond.sounds[sondIdx];
        bool isEmbedded = (sound->flags & 0x01) != 0;
        if (isEmbedded) continue;
        if (sondIdxToAudoIndex[sondIdx] >= 0) continue;
        if (sound->file == nullptr) continue;
        // Non-embedded audio files DO have an entry on the AUDO chunk, but we will ignore them because they are bogus entries
        const ExternalFile* match = nullptr;
        char nameOgg[256], nameWav[256];
        snprintf(nameOgg, sizeof(nameOgg), "%s.ogg", sound->file);
        snprintf(nameWav, sizeof(nameWav), "%s.wav", sound->file);
        repeat(options->externalAudioFileCount, fi) {
            const char* fname = options->externalAudioFiles[fi].fileName;
            if (strcmp(fname, sound->file) == 0 || strcmp(fname, nameOgg) == 0 || strcmp(fname, nameWav) == 0) {
                match = &options->externalAudioFiles[fi];
                break;
            }
        }
        if (match == nullptr) continue;
        AudioData decoded = AudioCodec_decode(match->bytes, match->size);
        printf("  Decoded %s%s\n", sound->file, decoded.valid ? "" : " (FAILED)");
        if (decoded.valid) {
            sondIdxToAudoIndex[sondIdx] = (int32_t) arrlen(parsedAudio);
            arrput(parsedAudio, decoded);
            externalCount++;
        }
    }

    int totalDecoded = 0;
    repeat(arrlen(parsedAudio), i) {
        if (parsedAudio[i].valid) totalDecoded++;
    }
    int failedCount = (int) arrlen(parsedAudio) - totalDecoded;
    printf("  %d embedded + %d from audiogroups + %d external = %d decoded sounds, %d failed or empty\n",
        embeddedCount, audioGroupCount, externalCount, totalDecoded, failedCount);

    // Process streamed music files (mus/ directory)
    AudioData* musAudio = nullptr; // dynarray
    char** musPaths = nullptr;     // dynarray of owned strdups
    if (options->musFileCount > 0) {
        printf("Decoding %zu streamed music files...\n", options->musFileCount);
        repeat(options->musFileCount, i) {
            const ExternalFile* f = &options->musFiles[i];
            AudioData decoded = AudioCodec_decode(f->bytes, f->size);
            if (decoded.valid) {
                printf("  %s: %dHz %dch -> ADPCM (%zu bytes)\n", f->fileName, decoded.sampleRate, decoded.channels, decoded.dataSize);
                arrput(musAudio, decoded);
                arrput(musPaths, safeStrdup(f->fileName));
            } else {
                printf("  %s: FAILED to decode\n", f->fileName);
            }
        }
        printf("  %td/%zu music files decoded\n", arrlen(musAudio), options->musFileCount);
    }

    // Build SOUNDS.BIN
    ByteWriter soundsWriter = ByteWriter_create(1024 * 1024);
    uint32_t* audioOffsets = safeCalloc(arrlen(parsedAudio) == 0 ? 1 : arrlen(parsedAudio), sizeof(uint32_t));
    uint32_t* audioSizes = safeCalloc(arrlen(parsedAudio) == 0 ? 1 : arrlen(parsedAudio), sizeof(uint32_t));
    repeat(arrlen(parsedAudio), i) {
        if (parsedAudio[i].valid) {
            audioOffsets[i] = (uint32_t) soundsWriter.size;
            audioSizes[i] = (uint32_t) parsedAudio[i].dataSize;
            ByteWriter_writeBytes(&soundsWriter, parsedAudio[i].data, parsedAudio[i].dataSize);
        }
    }
    uint32_t* musOffsets = safeCalloc(arrlen(musAudio) == 0 ? 1 : arrlen(musAudio), sizeof(uint32_t));
    uint32_t* musSizes = safeCalloc(arrlen(musAudio) == 0 ? 1 : arrlen(musAudio), sizeof(uint32_t));
    repeat(arrlen(musAudio), i) {
        musOffsets[i] = (uint32_t) soundsWriter.size;
        musSizes[i] = (uint32_t) musAudio[i].dataSize;
        ByteWriter_writeBytes(&soundsWriter, musAudio[i].data, musAudio[i].dataSize);
    }

    size_t soundsSize;
    uint8_t* soundsBin = ByteWriter_detach(&soundsWriter, &soundsSize);
    int pcmCount = 0, adpcmCount = 0;
    repeat(arrlen(parsedAudio), i) {
        if (!parsedAudio[i].valid) continue;
        if (parsedAudio[i].format == AUDIO_FORMAT_PCM) pcmCount++;
        else if (parsedAudio[i].format == AUDIO_FORMAT_ADPCM) adpcmCount++;
    }
    printf("  SOUNDS.BIN: %zu bytes (%d PCM, %d ADPCM, %td music tracks)\n", soundsSize, pcmCount, adpcmCount, arrlen(musAudio));

    size_t soundBnkSize;
    uint8_t* soundBnkBin = writeSoundBnkBytes(dw, parsedAudio, arrlen(parsedAudio), sondIdxToAudoIndex,
        musAudio, musPaths, arrlen(musAudio),
        audioOffsets, audioSizes, musOffsets, musSizes, &soundBnkSize);

    free(audioOffsets);
    free(audioSizes);
    free(musOffsets);
    free(musSizes);
    free(sondIdxToAudoIndex);

    // Build the result
    ProcessingResult result = {0};
    const char* gameName = dw->gen8.displayName ? dw->gen8.displayName : (dw->gen8.name ? dw->gen8.name : "GAME");
    result.gameName = safeStrdup(gameName);
    result.clut4Bin = clut4Bin;
    result.clut4Size = clut4Size;
    result.clut8Bin = clut8Bin;
    result.clut8Size = clut8Size;
    result.texturesBin = texturesBin;
    result.texturesSize = texturesSize;
    result.atlasBin = atlasBin;
    result.atlasSize = atlasBinSize;
    result.soundBnkBin = soundBnkBin;
    result.soundBnkSize = soundBnkSize;
    result.soundsBin = soundsBin;
    result.soundsSize = soundsSize;

    printf("Done!\n");

    // Cleanup
    repeat(arrlen(parsedAudio), i) AudioData_free(&parsedAudio[i]);
    arrfree(parsedAudio);
    repeat(arrlen(musAudio), i) AudioData_free(&musAudio[i]);
    arrfree(musAudio);
    repeat(arrlen(musPaths), i) free(musPaths[i]);
    arrfree(musPaths);

    repeat(maxGroupId + 1, i) {
        if (audioGroupDws[i] != nullptr) DataWin_free(audioGroupDws[i]);
    }
    free(audioGroupDws);

    repeat(imageCount, i) ClutImage_free(&clutImages[i]);
    free(clutImages);
    repeat(imageCount, i) free(collected[i].name);
    arrfree(collected);
    arrfree(uniqueTiles);
    free(cropByImage);

    // mergedGroups palettes are owned by us; free them
    repeat(mergedCount, i) ClutGroup_free(&mergedGroups[i]);
    free(mergedGroups);

    // atlases entries
    repeat(atlasCount, i) TextureAtlas_free(&atlases[i]);
    free(atlases);

    // Free shget hashmaps (we strdup'd both keys and values)
    repeat(shlen(atlasGroupKeyByName), i) {
        free(atlasGroupKeyByName[i].value);
    }
    shfree(atlasGroupKeyByName);
    shfree(tpagIndexByImageName);

    DataWin_free(dw);

    return result;
}
