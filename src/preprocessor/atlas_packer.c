#include "atlas_packer.h"
#include "utils.h"

#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ MaxRects bin packing algorithm (Best Short Side Fit heuristic) ]===
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} Rect;

typedef struct {
    int32_t binWidth;
    int32_t binHeight;
    Rect* freeRects; // stb_ds dynamic array
} MaxRectsPacker;

static MaxRectsPacker MaxRectsPacker_create(int32_t binWidth, int32_t binHeight) {
    MaxRectsPacker p = {.binWidth = binWidth, .binHeight = binHeight, .freeRects = nullptr};
    arrput(p.freeRects, ((Rect){.x = 0, .y = 0, .width = binWidth, .height = binHeight}));
    return p;
}

static MaxRectsPacker MaxRectsPacker_clone(const MaxRectsPacker* src) {
    MaxRectsPacker p = {.binWidth = src->binWidth, .binHeight = src->binHeight, .freeRects = nullptr};
    repeat(arrlen(src->freeRects), i) {
        arrput(p.freeRects, src->freeRects[i]);
    }
    return p;
}

static void MaxRectsPacker_free(MaxRectsPacker* p) {
    arrfree(p->freeRects);
    p->freeRects = nullptr;
}

static bool overlaps(const Rect* a, const Rect* b) {
    return a->x < b->x + b->width && a->x + a->width > b->x &&
            a->y < b->y + b->height && a->y + a->height > b->y;
}

static bool contains(const Rect* outer, const Rect* inner) {
    return outer->x <= inner->x && outer->y <= inner->y &&
            outer->x + outer->width >= inner->x + inner->width &&
            outer->y + outer->height >= inner->y + inner->height;
}

static void splitFreeRects(MaxRectsPacker* p, const Rect* used) {
    Rect* newRects = nullptr;
    ptrdiff_t writeIdx = 0;
    repeat(arrlen(p->freeRects), readIdx) {
        Rect* free_ = &p->freeRects[readIdx];
        if (!overlaps(free_, used)) {
            // Keep this rect in place
            if (writeIdx != (ptrdiff_t) readIdx) p->freeRects[writeIdx] = *free_;
            writeIdx++;
            continue;
        }
        // Left strip
        if (free_->x < used->x) {
            arrput(newRects, ((Rect){.x = free_->x, .y = free_->y, .width = used->x - free_->x, .height = free_->height}));
        }
        // Right strip
        if (free_->x + free_->width > used->x + used->width) {
            arrput(newRects, ((Rect){.x = used->x + used->width, .y = free_->y, .width = free_->x + free_->width - used->x - used->width, .height = free_->height}));
        }
        // Top strip
        if (free_->y < used->y) {
            arrput(newRects, ((Rect){.x = free_->x, .y = free_->y, .width = free_->width, .height = used->y - free_->y}));
        }
        // Bottom strip
        if (free_->y + free_->height > used->y + used->height) {
            arrput(newRects, ((Rect){.x = free_->x, .y = used->y + used->height, .width = free_->width, .height = free_->y + free_->height - used->y - used->height}));
        }
    }
    arrsetlen(p->freeRects, writeIdx);
    repeat(arrlen(newRects), i) {
        arrput(p->freeRects, newRects[i]);
    }
    arrfree(newRects);
}

static void pruneFreeRects(MaxRectsPacker* p) {
    // Remove any free rect that is fully contained within another
    ptrdiff_t size = arrlen(p->freeRects);
    ptrdiff_t writeIdx = 0;
    for (ptrdiff_t i = 0; i < size; i++) {
        bool contained = false;
        for (ptrdiff_t j = 0; j < size; j++) {
            if (i != j && contains(&p->freeRects[j], &p->freeRects[i])) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            p->freeRects[writeIdx++] = p->freeRects[i];
        }
    }
    arrsetlen(p->freeRects, writeIdx);
}

// Try to insert a w x h rect. Returns true and writes (outX, outY) on success, false if it doesn't fit.
static bool MaxRectsPacker_insert(MaxRectsPacker* p, int32_t w, int32_t h, int32_t* outX, int32_t* outY) {
    int32_t bestX = -1, bestY = -1;
    int32_t bestShortSide = INT32_MAX;
    int32_t bestLongSide = INT32_MAX;

    repeat(arrlen(p->freeRects), i) {
        Rect* rect = &p->freeRects[i];
        if (rect->width >= w && rect->height >= h) {
            int32_t leftoverHoriz = rect->width - w;
            int32_t leftoverVert = rect->height - h;
            int32_t shortSide = leftoverHoriz < leftoverVert ? leftoverHoriz : leftoverVert;
            int32_t longSide = leftoverHoriz > leftoverVert ? leftoverHoriz : leftoverVert;
            if (shortSide < bestShortSide || (shortSide == bestShortSide && bestLongSide > longSide)) {
                bestX = rect->x;
                bestY = rect->y;
                bestShortSide = shortSide;
                bestLongSide = longSide;
            }
        }
    }

    if (bestX == -1) return false;

    Rect placed = {.x = bestX, .y = bestY, .width = w, .height = h};
    splitFreeRects(p, &placed);
    pruneFreeRects(p);
    *outX = bestX;
    *outY = bestY;
    return true;
}

// ===[ TextureAtlas ]===

void TextureAtlas_free(TextureAtlas* atlas) {
    free(atlas->entries);
    atlas->entries = nullptr;
    atlas->entryCount = 0;
    atlas->entryCapacity = 0;
}

static void TextureAtlas_appendEntry(TextureAtlas* atlas, int32_t imageIndex, uint32_t x, uint32_t y) {
    if (atlas->entryCount + 1 > atlas->entryCapacity) {
        uint32_t newCap = atlas->entryCapacity == 0 ? 16 : atlas->entryCapacity * 2;
        atlas->entries = safeRealloc(atlas->entries, newCap * sizeof(AtlasEntry));
        atlas->entryCapacity = newCap;
    }
    atlas->entries[atlas->entryCount++] = (AtlasEntry){.imageIndex = imageIndex, .x = x, .y = y};
}

// ===[ Group bookkeeping ]===

typedef struct {
    char* key;            // owned
    int32_t* imageIndices; // stb_ds dynamic array
    uint64_t totalArea;
} ImageGroup;

static int compareGroupsByAreaDesc(const void* a, const void* b) {
    const ImageGroup* ga = (const ImageGroup*) a;
    const ImageGroup* gb = (const ImageGroup*) b;
    if (gb->totalArea > ga->totalArea) return 1;
    if (ga->totalArea > gb->totalArea) return -1;
    return 0;
}

static int compareImagesByLongestSideDesc(const void* a, const void* b, void* arg) {
    int32_t ia = *(const int32_t*) a;
    int32_t ib = *(const int32_t*) b;
    ClutImage* images = (ClutImage*) arg;
    uint32_t la = images[ia].width > images[ia].height ? images[ia].width : images[ia].height;
    uint32_t lb = images[ib].width > images[ib].height ? images[ib].width : images[ib].height;
    if (lb > la) return 1;
    if (la > lb) return -1;
    return 0;
}

// Plain qsort doesn't accept a context pointer, so we route comparators through a single global. Image counts are bounded and the packer is single-threaded.
typedef int (*PortableCmp)(const void*, const void*, void*);
static PortableCmp g_cmp;
static void* g_arg;
static int trampoline(const void* a, const void* b) { return g_cmp(a, b, g_arg); }
static void qsort_r_portable(void* base, size_t nmemb, size_t size, int (*cmp)(const void*, const void*, void*), void* arg) {
    g_cmp = cmp; g_arg = arg;
    qsort(base, nmemb, size, trampoline);
}

static void packByBpp(
    ClutImage* images,
    int32_t* imageIndices, size_t imageIndexCount,
    uint8_t bpp,
    AtlasGroupEntry* groupEntries, size_t groupEntryCount,
    TextureAtlas** atlases, size_t* atlasCount, size_t* atlasCapacity,
    MaxRectsPacker** packers, size_t* packerCount, size_t* packerCapacity)
{
    if (imageIndexCount == 0) return;

    // Build a name -> group-key lookup from groupEntries
    struct { const char* key; const char* value; }* nameToGroupKey = nullptr;
    repeat(groupEntryCount, i) {
        shput(nameToGroupKey, groupEntries[i].imageName, groupEntries[i].groupKey);
    }

    // Group images by their group key (insertion-ordered)
    ImageGroup* groups = nullptr; // dynamic array
    struct { char* key; int32_t value; }* groupIndexByKey = nullptr; // stb_ds string-keyed map of group index

    repeat(imageIndexCount, i) {
        int32_t imgIdx = imageIndices[i];
        ClutImage* img = &images[imgIdx];
        const char* groupKey;
        ptrdiff_t gki = shgeti(nameToGroupKey, img->name);
        groupKey = (0 <= gki) ? nameToGroupKey[gki].value : img->name;

        ptrdiff_t existing = shgeti(groupIndexByKey, groupKey);
        int32_t groupIdx;
        if (0 <= existing) {
            groupIdx = groupIndexByKey[existing].value;
        } else {
            ImageGroup g = {.key = safeStrdup(groupKey), .imageIndices = nullptr, .totalArea = 0};
            arrput(groups, g);
            groupIdx = (int32_t)(arrlen(groups) - 1);
            shput(groupIndexByKey, groups[groupIdx].key, groupIdx);
        }
        arrput(groups[groupIdx].imageIndices, imgIdx);
        groups[groupIdx].totalArea += (uint64_t) img->width * img->height;
    }
    shfree(groupIndexByKey);
    shfree(nameToGroupKey);

    // Sort groups by total area descending (pack large groups first for better utilization)
    qsort(groups, arrlen(groups), sizeof(ImageGroup), compareGroupsByAreaDesc);

    ptrdiff_t groupCount = arrlen(groups);
    repeat(groupCount, gi) {
        ImageGroup* group = &groups[gi];
        if (gi % 100 == 0) {
            printf("    Group %td/%td (%td images, %zu atlases so far)\n", (ptrdiff_t) gi, groupCount, arrlen(group->imageIndices), *atlasCount);
        }

        // Sort images within group by max(width, height) descending for better packing
        qsort_r_portable(group->imageIndices, arrlen(group->imageIndices), sizeof(int32_t), compareImagesByLongestSideDesc, images);

        // Try to fit the entire group into one existing atlas (same bpp)
        bool packed = false;
        repeat(*atlasCount, ai) {
            TextureAtlas* atlas = &(*atlases)[ai];
            if (atlas->bpp != bpp) continue;
            MaxRectsPacker* packer = &(*packers)[ai];

            // Speculatively try on a clone of the packer
            MaxRectsPacker clonedPacker = MaxRectsPacker_clone(packer);
            int32_t* placementsX = nullptr;
            int32_t* placementsY = nullptr;
            int32_t* placementsImg = nullptr;
            bool allFit = true;
            repeat(arrlen(group->imageIndices), si) {
                ClutImage* img = &images[group->imageIndices[si]];
                int32_t x, y;
                if (!MaxRectsPacker_insert(&clonedPacker, (int32_t) img->width, (int32_t) img->height, &x, &y)) {
                    allFit = false;
                    break;
                }
                arrput(placementsX, x);
                arrput(placementsY, y);
                arrput(placementsImg, group->imageIndices[si]);
            }

            if (allFit) {
                // Commit: replace the packer with the clone and add entries
                MaxRectsPacker_free(packer);
                *packer = clonedPacker;
                repeat(arrlen(placementsX), pi) {
                    TextureAtlas_appendEntry(atlas, placementsImg[pi], (uint32_t) placementsX[pi], (uint32_t) placementsY[pi]);
                }
                packed = true;
            } else {
                MaxRectsPacker_free(&clonedPacker);
            }
            arrfree(placementsX);
            arrfree(placementsY);
            arrfree(placementsImg);
            if (packed) break;
        }

        if (!packed) {
            // Create new atlas(es) for this group
            int32_t* remaining = nullptr;
            repeat(arrlen(group->imageIndices), si) {
                arrput(remaining, group->imageIndices[si]);
            }
            while (arrlen(remaining) > 0) {
                if (*atlasCount + 1 > *atlasCapacity) {
                    *atlasCapacity = *atlasCapacity == 0 ? 16 : *atlasCapacity * 2;
                    *atlases = safeRealloc(*atlases, *atlasCapacity * sizeof(TextureAtlas));
                }
                if (*packerCount + 1 > *packerCapacity) {
                    *packerCapacity = *packerCapacity == 0 ? 16 : *packerCapacity * 2;
                    *packers = safeRealloc(*packers, *packerCapacity * sizeof(MaxRectsPacker));
                }
                TextureAtlas atlas = {.id = (int32_t) *atlasCount, .bpp = bpp, .width = ATLAS_PACKER_MAX_SIZE, .height = ATLAS_PACKER_MAX_SIZE, .entries = nullptr, .entryCount = 0, .entryCapacity = 0};
                MaxRectsPacker packer = MaxRectsPacker_create(ATLAS_PACKER_MAX_SIZE, ATLAS_PACKER_MAX_SIZE);

                // Try to fit each remaining image; collect the failures and keep them for the next atlas
                int32_t* stillRemaining = nullptr;
                repeat(arrlen(remaining), ri) {
                    int32_t imgIdx = remaining[ri];
                    ClutImage* img = &images[imgIdx];
                    int32_t x, y;
                    if (MaxRectsPacker_insert(&packer, (int32_t) img->width, (int32_t) img->height, &x, &y)) {
                        TextureAtlas_appendEntry(&atlas, imgIdx, (uint32_t) x, (uint32_t) y);
                    } else {
                        arrput(stillRemaining, imgIdx);
                    }
                }
                arrfree(remaining);
                remaining = stillRemaining;

                if (atlas.entryCount == 0) {
                    TextureAtlas_free(&atlas);
                    MaxRectsPacker_free(&packer);
                    break;
                }
                (*atlases)[(*atlasCount)++] = atlas;
                (*packers)[(*packerCount)++] = packer;
            }
            arrfree(remaining);
        }
    }

    repeat(arrlen(groups), gi) {
        free(groups[gi].key);
        arrfree(groups[gi].imageIndices);
    }
    arrfree(groups);
}

TextureAtlas* TextureAtlasPacker_packAtlases(ClutImage* images, size_t imageCount, AtlasGroupEntry* groupEntries, size_t groupEntryCount, size_t* outAtlasCount) {
    int32_t* indices4 = nullptr;
    int32_t* indices8 = nullptr;
    repeat(imageCount, i) {
        if (images[i].bpp == 4) arrput(indices4, (int32_t) i);
        else arrput(indices8, (int32_t) i);
    }
    printf("  packAtlases: %td 4bpp images, %td 8bpp images\n", arrlen(indices4), arrlen(indices8));

    TextureAtlas* atlases = nullptr;
    size_t atlasCount = 0;
    size_t atlasCapacity = 0;
    MaxRectsPacker* packers = nullptr;
    size_t packerCount = 0;
    size_t packerCapacity = 0;

    printf("  Packing 4bpp...\n");
    packByBpp(images, indices4, arrlen(indices4), 4, groupEntries, groupEntryCount, &atlases, &atlasCount, &atlasCapacity, &packers, &packerCount, &packerCapacity);
    printf("  Packing 8bpp...\n");
    packByBpp(images, indices8, arrlen(indices8), 8, groupEntries, groupEntryCount, &atlases, &atlasCount, &atlasCapacity, &packers, &packerCount, &packerCapacity);
    printf("  Packing done.\n");

    arrfree(indices4);
    arrfree(indices8);

    repeat(packerCount, i) MaxRectsPacker_free(&packers[i]);
    free(packers);

    *outAtlasCount = atlasCount;
    return atlases;
}
