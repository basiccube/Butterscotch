#include "clut_processor.h"
#include "median_cut.h"
#include "utils.h"

#include "stb_ds.h"

#include <stdlib.h>
#include <string.h>

// ===[ Helpers ]===

static int compareUnsignedU32(const void* a, const void* b) {
    uint32_t ua = *(const uint32_t*) a;
    uint32_t ub = *(const uint32_t*) b;
    if (ub > ua) return -1;
    if (ua > ub) return 1;
    return 0;
}

static void sortUnsigned(uint32_t* arr, size_t len) {
    qsort(arr, len, sizeof(uint32_t), compareUnsignedU32);
}

// FNV-1a hash over the used portion of a palette plus the usedColors count, used as the dedup hashmap key
static uint64_t paletteHash(const uint32_t* palette, uint32_t usedColors) {
    uint64_t h = 1469598103934665603ULL;
    h = (h ^ (uint64_t) usedColors) * 1099511628211ULL;
    repeat(usedColors, i) {
        uint64_t v = (uint64_t) palette[i];
        h = (h ^ v) * 1099511628211ULL;
    }
    return h;
}

// ===[ Lifecycle ]===

void ClutImage_free(ClutImage* image) {
    free(image->name);
    if (image->ownsPalette) free(image->palette);
    free(image->indices);
    image->name = nullptr;
    image->palette = nullptr;
    image->indices = nullptr;
}

void ClutGroup_free(ClutGroup* group) {
    free(group->colors);
    free(group->palette);
    free(group->imageIndices);
    group->colors = nullptr;
    group->palette = nullptr;
    group->imageIndices = nullptr;
}

uint32_t ClutImage_getUsedColors(const ClutImage* image, uint32_t* outColors) {
    repeat(image->usedColors, i) {
        outColors[i] = image->palette[i];
    }
    return image->usedColors;
}

// ===[ createClutImage ]===

// Direct mapping when the image already has <=16 (or <=256) unique colors. The palette is just the sorted unique colors.
static ClutImage buildDirectClut(const char* name, uint32_t w, uint32_t h, const uint32_t* pixels, size_t pixelCount, const uint32_t* uniqueColors, size_t uniqueCount, uint8_t bpp) {
    uint32_t maxSlots = (bpp == 4) ? 16 : 256;
    uint32_t* sorted = safeMalloc(uniqueCount * sizeof(uint32_t));
    memcpy(sorted, uniqueColors, uniqueCount * sizeof(uint32_t));
    sortUnsigned(sorted, uniqueCount);

    uint32_t* palette = safeCalloc(maxSlots, sizeof(uint32_t));
    repeat(uniqueCount, i) palette[i] = sorted[i];

    // colorToIndex hashmap from palette color -> palette slot
    struct { uint32_t key; int32_t value; }* colorToIndex = nullptr;
    repeat(uniqueCount, i) hmput(colorToIndex, sorted[i], (int32_t) i);

    uint8_t* indices = safeMalloc(pixelCount);
    repeat(pixelCount, i) {
        ptrdiff_t idx = hmgeti(colorToIndex, pixels[i]);
        indices[i] = (uint8_t) colorToIndex[idx].value;
    }
    hmfree(colorToIndex);
    free(sorted);

    return (ClutImage){
        .name = safeStrdup(name),
        .width = w,
        .height = h,
        .bpp = bpp,
        .palette = palette,
        .paletteSlots = maxSlots,
        .usedColors = (uint32_t) uniqueCount,
        .indices = indices,
        .ownsPalette = true
    };
}

// Nearest-color search by squared RGB distance, skipping the transparent slot.
static int32_t nearestOpaqueSortedIdx(uint32_t argb, const uint32_t* sortedColors, uint32_t sortedCount, bool hasTransparent) {
    int r = (int) ((argb >> 16) & 0xFF);
    int g = (int) ((argb >> 8) & 0xFF);
    int b = (int) (argb & 0xFF);
    uint32_t start = hasTransparent ? 1 : 0;
    int32_t bestIdx = (int32_t) start;
    int bestDist = INT32_MAX;
    for (uint32_t i = start; i < sortedCount; i++) {
        uint32_t c = sortedColors[i];
        int dr = (int) ((c >> 16) & 0xFF) - r;
        int dg = (int) ((c >> 8) & 0xFF) - g;
        int db = (int) (c & 0xFF) - b;
        int d = dr * dr + dg * dg + db * db;
        if (bestDist > d) {
            bestDist = d;
            bestIdx = (int32_t) i;
        }
    }
    return bestIdx;
}

// Quantize via median-cut when the image has more colors than fit in the palette.
static ClutImage buildQuantizedClut(const char* name, uint32_t w, uint32_t h, const uint32_t* pixels, size_t pixelCount, const uint32_t* uniqueColors, size_t uniqueCount, uint8_t targetBpp) {
    uint32_t paletteSlots = (targetBpp == 4) ? 16 : 256;
    bool hasTransparent = false;
    repeat(uniqueCount, i) {
        if (uniqueColors[i] == 0) { hasTransparent = true; break; }
    }

    // opaquePixels: pixels with non-zero alpha (or all if hasTransparent is false)
    uint32_t* opaquePixels;
    size_t opaqueCount;
    if (hasTransparent) {
        opaquePixels = safeMalloc(pixelCount * sizeof(uint32_t));
        opaqueCount = 0;
        repeat(pixelCount, i) {
            if ((pixels[i] >> 24) != 0) {
                opaquePixels[opaqueCount++] = pixels[i];
            }
        }
    } else {
        opaquePixels = safeMalloc(pixelCount * sizeof(uint32_t));
        memcpy(opaquePixels, pixels, pixelCount * sizeof(uint32_t));
        opaqueCount = pixelCount;
    }

    if (opaqueCount == 0) {
        free(opaquePixels);
        uint32_t* palette = safeCalloc(paletteSlots, sizeof(uint32_t));
        return (ClutImage){
            .name = safeStrdup(name),
            .width = w,
            .height = h,
            .bpp = targetBpp,
            .palette = palette,
            .paletteSlots = paletteSlots,
            .usedColors = 1,
            .indices = safeCalloc(pixelCount, 1),
            .ownsPalette = true
        };
    }

    uint32_t maxQuantColors = hasTransparent ? paletteSlots - 1 : paletteSlots;
    uint32_t* quantPalette = safeMalloc(maxQuantColors * sizeof(uint32_t));
    size_t quantCount = MedianCut_quantize(opaquePixels, opaqueCount, maxQuantColors, quantPalette);
    free(opaquePixels);

    // Sort canonically by unsigned ARGB to match the rest of the pipeline
    size_t unsortedCount = quantCount + (hasTransparent ? 1 : 0);
    uint32_t* sortedColors = safeMalloc(unsortedCount * sizeof(uint32_t));
    size_t pos = 0;
    if (hasTransparent) sortedColors[pos++] = 0x00000000;
    repeat(quantCount, i) sortedColors[pos++] = quantPalette[i];
    free(quantPalette);
    sortUnsigned(sortedColors, unsortedCount);

    uint32_t* palette = safeCalloc(paletteSlots, sizeof(uint32_t));
    repeat(unsortedCount, i) palette[i] = sortedColors[i];

    // Cache nearest-color lookups; sprites usually have heavy color repetition
    struct { uint32_t key; int32_t value; }* cache = nullptr;
    uint8_t* indices = safeMalloc(pixelCount);
    repeat(pixelCount, idx) {
        uint32_t argb = pixels[idx];
        if ((argb >> 24) == 0) {
            // sortedColors[0] is 0x00000000 when hasTransparent; otherwise this branch is unreachable
            indices[idx] = 0;
        } else {
            ptrdiff_t cachedIdx = hmgeti(cache, argb);
            if (0 <= cachedIdx) {
                indices[idx] = (uint8_t) cache[cachedIdx].value;
            } else {
                int32_t n = nearestOpaqueSortedIdx(argb, sortedColors, (uint32_t) unsortedCount, hasTransparent);
                hmput(cache, argb, n);
                indices[idx] = (uint8_t) n;
            }
        }
    }
    hmfree(cache);
    free(sortedColors);

    return (ClutImage){
        .name = safeStrdup(name),
        .width = w,
        .height = h,
        .bpp = targetBpp,
        .palette = palette,
        .paletteSlots = paletteSlots,
        .usedColors = (uint32_t) unsortedCount,
        .indices = indices,
        .ownsPalette = true
    };
}

ClutImage ClutProcessor_createClutImage(const char* name, const PixelImage* image, bool force4bpp) {
    uint32_t w = image->width;
    uint32_t h = image->height;
    size_t pixelCount = (size_t) w * h;

    // Normalize transparent pixels to 0x00000000
    uint32_t* pixels = safeMalloc(pixelCount * sizeof(uint32_t));
    memcpy(pixels, image->pixels, pixelCount * sizeof(uint32_t));
    repeat(pixelCount, i) {
        if ((pixels[i] >> 24) == 0) pixels[i] = 0;
    }

    // Collect unique colors via stb_ds set (we use a uint32_t key map and ignore the value)
    struct { uint32_t key; uint8_t value; }* uniqueSet = nullptr;
    repeat(pixelCount, i) hmput(uniqueSet, pixels[i], 0);
    size_t uniqueCount = (size_t) hmlen(uniqueSet);
    uint32_t* uniqueColors = safeMalloc(uniqueCount * sizeof(uint32_t));
    repeat(uniqueCount, i) uniqueColors[i] = uniqueSet[i].key;
    hmfree(uniqueSet);

    ClutImage result;
    if (force4bpp) {
        if (16 >= uniqueCount) {
            result = buildDirectClut(name, w, h, pixels, pixelCount, uniqueColors, uniqueCount, 4);
        } else {
            result = buildQuantizedClut(name, w, h, pixels, pixelCount, uniqueColors, uniqueCount, 4);
        }
    } else if (16 >= uniqueCount) {
        result = buildDirectClut(name, w, h, pixels, pixelCount, uniqueColors, uniqueCount, 4);
    } else if (256 >= uniqueCount) {
        result = buildDirectClut(name, w, h, pixels, pixelCount, uniqueColors, uniqueCount, 8);
    } else {
        result = buildQuantizedClut(name, w, h, pixels, pixelCount, uniqueColors, uniqueCount, 8);
    }

    free(pixels);
    free(uniqueColors);
    return result;
}

// ===[ Deduplicate ]===

static void ClutGroup_appendImageIndex(ClutGroup* group, int32_t imageIndex) {
    if (group->imageIndexCount + 1 > group->imageIndexCapacity) {
        uint32_t newCap = group->imageIndexCapacity == 0 ? 4 : group->imageIndexCapacity * 2;
        group->imageIndices = safeRealloc(group->imageIndices, newCap * sizeof(int32_t));
        group->imageIndexCapacity = newCap;
    }
    group->imageIndices[group->imageIndexCount++] = imageIndex;
}

ClutGroup* ClutProcessor_deduplicateCluts(ClutImage* images, size_t imageCount, size_t* outGroupCount) {
    // Hashmap of palette-hash -> dynamic array of group indices that hashed to that bucket. Equality verified by full byte compare to handle collisions.
    struct { uint64_t key; int32_t* value; /* stb_ds dynarray of group indices */ }* hashBuckets = nullptr;

    ClutGroup* groups = nullptr;
    size_t groupCount = 0;
    size_t groupCapacity = 0;

    repeat(imageCount, i) {
        ClutImage* img = &images[i];
        uint64_t hash = paletteHash(img->palette, img->usedColors);
        ptrdiff_t bucketIdx = hmgeti(hashBuckets, hash);

        int32_t matchedGroupIdx = -1;
        if (0 <= bucketIdx) {
            int32_t* candidates = hashBuckets[bucketIdx].value;
            for (int32_t bi = 0; bi < arrlen(candidates); bi++) {
                int32_t gi = candidates[bi];
                ClutGroup* g = &groups[gi];
                if (g->bpp != img->bpp) continue;
                if (g->colorCount != img->usedColors) continue;
                bool eq = true;
                repeat(img->usedColors, j) {
                    if (g->colors[j] != img->palette[j]) { eq = false; break; }
                }
                if (eq) { matchedGroupIdx = gi; break; }
            }
        }

        if (0 > matchedGroupIdx) {
            if (groupCount + 1 > groupCapacity) {
                groupCapacity = groupCapacity == 0 ? 16 : groupCapacity * 2;
                groups = safeRealloc(groups, groupCapacity * sizeof(ClutGroup));
            }
            ClutGroup* g = &groups[groupCount];
            g->id = (int32_t) groupCount;
            g->bpp = img->bpp;
            g->colorCount = img->usedColors;
            g->colors = safeMalloc(img->usedColors * sizeof(uint32_t));
            repeat(img->usedColors, j) g->colors[j] = img->palette[j];
            g->palette = safeMalloc(img->paletteSlots * sizeof(uint32_t));
            memcpy(g->palette, img->palette, img->paletteSlots * sizeof(uint32_t));
            g->imageIndices = nullptr;
            g->imageIndexCount = 0;
            g->imageIndexCapacity = 0;
            ClutGroup_appendImageIndex(g, (int32_t) i);
            matchedGroupIdx = (int32_t) groupCount;
            groupCount++;

            if (0 > bucketIdx) {
                int32_t* newBucket = nullptr;
                arrput(newBucket, matchedGroupIdx);
                hmput(hashBuckets, hash, newBucket);
            } else {
                arrput(hashBuckets[bucketIdx].value, matchedGroupIdx);
            }
        } else {
            ClutGroup_appendImageIndex(&groups[matchedGroupIdx], (int32_t) i);
        }

        // Image now shares the group's palette
        if (img->ownsPalette) free(img->palette);
        img->palette = groups[matchedGroupIdx].palette;
        img->ownsPalette = false;
    }

    for (ptrdiff_t b = 0; b < hmlen(hashBuckets); b++) {
        arrfree(hashBuckets[b].value);
    }
    hmfree(hashBuckets);

    *outGroupCount = groupCount;
    return groups;
}

// ===[ Merge ]===

// Sorted merge of two sorted IntArrays, returns a new sorted IntArray with unique values.
// Both inputs must be sorted by unsigned value. Caller must free the returned array.
static uint32_t* sortedUnion(const uint32_t* a, uint32_t aLen, const uint32_t* b, uint32_t bLen, uint32_t* outLen) {
    uint32_t* result = safeMalloc((aLen + bLen) * sizeof(uint32_t));
    uint32_t ai = 0, bi = 0, ri = 0;
    while (aLen > ai && bLen > bi) {
        uint32_t av = a[ai];
        uint32_t bv = b[bi];
        if (bv > av) {
            result[ri++] = a[ai++];
        } else if (av > bv) {
            result[ri++] = b[bi++];
        } else {
            result[ri++] = a[ai++];
            bi++;
        }
    }
    while (aLen > ai) result[ri++] = a[ai++];
    while (bLen > bi) result[ri++] = b[bi++];
    *outLen = ri;
    return result;
}

// Count the union size of two sorted IntArrays (by unsigned value) with early exit.
// Returns UINT32_MAX if union would exceed limit.
static uint32_t sortedUnionSize(const uint32_t* a, uint32_t aLen, const uint32_t* b, uint32_t bLen, uint32_t limit) {
    uint32_t ai = 0, bi = 0;
    uint32_t count = 0;
    while (aLen > ai && bLen > bi) {
        uint32_t av = a[ai];
        uint32_t bv = b[bi];
        count++;
        if (count > limit) return UINT32_MAX;
        if (bv > av) ai++;
        else if (av > bv) bi++;
        else { ai++; bi++; }
    }
    count += (aLen - ai) + (bLen - bi);
    return count > limit ? UINT32_MAX : count;
}

typedef struct {
    int32_t id;
    uint8_t bpp;
    uint32_t* colors;     // sorted by unsigned value (owned)
    uint32_t colorCount;  // number of used entries in colors
    int32_t* imageIndices;
    uint32_t imageIndexCount;
    uint32_t imageIndexCapacity;
    bool alive;
} MergeableClut;

static void MergeableClut_appendImageIndex(MergeableClut* m, int32_t idx) {
    if (m->imageIndexCount + 1 > m->imageIndexCapacity) {
        uint32_t newCap = m->imageIndexCapacity == 0 ? 4 : m->imageIndexCapacity * 2;
        m->imageIndices = safeRealloc(m->imageIndices, newCap * sizeof(int32_t));
        m->imageIndexCapacity = newCap;
    }
    m->imageIndices[m->imageIndexCount++] = idx;
}

static void remapIndices(ClutImage* img, uint32_t* newPalette, uint32_t newPaletteSlots, uint32_t newUsedColors) {
    uint32_t* oldPalette = img->palette;
    uint32_t oldUsed = img->usedColors;

    // Build old index -> new index lookup table (avoids HashMap boxing)
    int32_t* remapTable = safeMalloc(oldUsed * sizeof(int32_t));
    repeat(oldUsed, oldIdx) {
        uint32_t color = oldPalette[oldIdx];
        // Binary search in the new palette (sorted by unsigned value)
        int32_t lo = 0;
        int32_t hi = (int32_t) newUsedColors - 1;
        int32_t found = (int32_t) oldIdx; // fallback
        while (hi >= lo) {
            int32_t mid = (lo + hi) >> 1;
            uint32_t mv = newPalette[mid];
            if (color > mv) lo = mid + 1;
            else if (mv > color) hi = mid - 1;
            else { found = mid; break; }
        }
        remapTable[oldIdx] = found;
    }

    size_t pixelCount = (size_t) img->width * img->height;
    repeat(pixelCount, i) {
        uint32_t oldIdx = (uint32_t) img->indices[i];
        if (oldUsed > oldIdx) {
            img->indices[i] = (uint8_t) remapTable[oldIdx];
        }
    }
    free(remapTable);

    img->palette = newPalette;
    img->paletteSlots = newPaletteSlots;
    img->usedColors = newUsedColors;
    img->ownsPalette = false;
}

static int compareMergeableByColorCount(const void* a, const void* b) {
    const MergeableClut* ma = *(const MergeableClut* const*) a;
    const MergeableClut* mb = *(const MergeableClut* const*) b;
    if (ma->colorCount > mb->colorCount) return 1;
    if (mb->colorCount > ma->colorCount) return -1;
    // Tie-break by id so the result is deterministic regardless of qsort's stability (Kotlin uses sortedBy which is stable on insertion order; ids reflect insertion order)
    if (ma->id > mb->id) return 1;
    if (mb->id > ma->id) return -1;
    return 0;
}

// Intermediate palettes allocated during merge are kept alive until ClutImages are repointed at the final palettes (built after mergeByBpp completes).
typedef struct {
    uint32_t** items;
    size_t count;
    size_t capacity;
} PaletteLeakList;

static void PaletteLeakList_add(PaletteLeakList* list, uint32_t* p) {
    if (list->count + 1 > list->capacity) {
        list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        list->items = safeRealloc(list->items, list->capacity * sizeof(uint32_t*));
    }
    list->items[list->count++] = p;
}

static void PaletteLeakList_free(PaletteLeakList* list) {
    repeat(list->count, i) free(list->items[i]);
    free(list->items);
}

static void mergeByBpp(MergeableClut* cluts, uint32_t clutCount, uint32_t maxSlots, ClutImage* images, PaletteLeakList* leaks) {
    bool changed = true;
    while (changed) {
        changed = false;

        // Snapshot of currently-alive cluts sorted by colorCount ascending
        MergeableClut** aliveCluts = safeMalloc(clutCount * sizeof(MergeableClut*));
        uint32_t aliveCount = 0;
        repeat(clutCount, i) {
            if (cluts[i].alive) aliveCluts[aliveCount++] = &cluts[i];
        }
        qsort(aliveCluts, aliveCount, sizeof(MergeableClut*), compareMergeableByColorCount);

        repeat(aliveCount, i) {
            MergeableClut* clutA = aliveCluts[i];
            if (!clutA->alive) continue;
            if (clutA->colorCount >= maxSlots) continue;

            MergeableClut* bestPartner = nullptr;
            uint32_t bestUnionSize = UINT32_MAX;

            repeat(aliveCount, j) {
                if (i == j) continue;
                MergeableClut* clutB = aliveCluts[j];
                if (!clutB->alive) continue;

                uint32_t limit = maxSlots;
                if (bestUnionSize - 1 < limit) limit = bestUnionSize - 1;
                uint32_t unionSize = sortedUnionSize(
                    clutA->colors, clutA->colorCount,
                    clutB->colors, clutB->colorCount,
                    limit
                );

                if (maxSlots >= unionSize && bestUnionSize > unionSize) {
                    bestUnionSize = unionSize;
                    bestPartner = clutB;
                    // Perfect subset - can't do better
                    uint32_t maxAB = clutA->colorCount > clutB->colorCount ? clutA->colorCount : clutB->colorCount;
                    if (unionSize == maxAB) break;
                }
            }

            if (bestPartner != nullptr) {
                uint32_t mergedLen;
                uint32_t* merged = sortedUnion(
                    bestPartner->colors, bestPartner->colorCount,
                    clutA->colors, clutA->colorCount,
                    &mergedLen
                );
                uint32_t* newPalette = safeCalloc(maxSlots, sizeof(uint32_t));
                repeat(mergedLen, k) newPalette[k] = merged[k];

                repeat(bestPartner->imageIndexCount, k) {
                    remapIndices(&images[bestPartner->imageIndices[k]], newPalette, maxSlots, mergedLen);
                }
                repeat(clutA->imageIndexCount, k) {
                    remapIndices(&images[clutA->imageIndices[k]], newPalette, maxSlots, mergedLen);
                }

                // bestPartner now owns the merged colors and palette; clutA is retired
                free(bestPartner->colors);
                bestPartner->colors = merged;
                bestPartner->colorCount = mergedLen;
                repeat(clutA->imageIndexCount, k) {
                    MergeableClut_appendImageIndex(bestPartner, clutA->imageIndices[k]);
                }
                clutA->alive = false;
                // newPalette is now referenced by every image in the merged group via img->palette. We can't free it yet (subsequent merges may read those palettes via remapIndices), so park it in the leak list and free after merging completes.
                PaletteLeakList_add(leaks, newPalette);
                changed = true;
            }
        }

        free(aliveCluts);
    }
}

ClutGroup* ClutProcessor_mergeCluts(ClutImage* images, MAYBE_UNUSED size_t imageCount, ClutGroup* initialGroups, size_t initialGroupCount, size_t* outGroupCount) {
    // Separate by bpp upfront so we never compare across bpp
    uint32_t count4 = 0, count8 = 0;
    repeat(initialGroupCount, i) {
        if (initialGroups[i].bpp == 4) count4++;
        else count8++;
    }
    MergeableClut* groups4 = safeCalloc(count4 == 0 ? 1 : count4, sizeof(MergeableClut));
    MergeableClut* groups8 = safeCalloc(count8 == 0 ? 1 : count8, sizeof(MergeableClut));
    uint32_t i4 = 0, i8 = 0;
    repeat(initialGroupCount, i) {
        ClutGroup* g = &initialGroups[i];
        MergeableClut* dst = (g->bpp == 4) ? &groups4[i4++] : &groups8[i8++];
        dst->id = g->id;
        dst->bpp = g->bpp;
        // Already sorted by unsigned value (since createClutImage sorts) — copy
        dst->colors = safeMalloc(g->colorCount * sizeof(uint32_t));
        memcpy(dst->colors, g->colors, g->colorCount * sizeof(uint32_t));
        dst->colorCount = g->colorCount;
        dst->imageIndexCount = 0;
        dst->imageIndexCapacity = g->imageIndexCount > 0 ? g->imageIndexCount : 4;
        dst->imageIndices = safeMalloc(dst->imageIndexCapacity * sizeof(int32_t));
        repeat(g->imageIndexCount, k) {
            MergeableClut_appendImageIndex(dst, g->imageIndices[k]);
        }
        dst->alive = true;
    }

    // initialGroups' palettes are still referenced by ClutImage->palette pointers (set during dedup); they must stay alive until after mergeByBpp completes (remapIndices reads img->palette to look up the old colors).

    PaletteLeakList leaks = {0};
    mergeByBpp(groups4, count4, 16, images, &leaks);
    mergeByBpp(groups8, count8, 256, images, &leaks);

    // Build final ClutGroups
    size_t finalCapacity = 16;
    ClutGroup* finalGroups = safeMalloc(finalCapacity * sizeof(ClutGroup));
    size_t finalCount = 0;

    MergeableClut* allGroups[2] = { groups4, groups8 };
    uint32_t allCounts[2] = { count4, count8 };
    for (uint32_t arrIdx = 0; arrIdx < 2; arrIdx++) {
        MergeableClut* arr = allGroups[arrIdx];
        uint32_t c = allCounts[arrIdx];
        repeat(c, i) {
            MergeableClut* m = &arr[i];
            if (!m->alive) {
                free(m->colors);
                free(m->imageIndices);
                continue;
            }
            uint32_t maxSlots = (m->bpp == 4) ? 16 : 256;
            uint32_t* palette = safeCalloc(maxSlots, sizeof(uint32_t));
            repeat(m->colorCount, k) palette[k] = m->colors[k];

            // All images in this merged group must point at this newly-allocated palette
            repeat(m->imageIndexCount, k) {
                ClutImage* img = &images[m->imageIndices[k]];
                if (img->ownsPalette) free(img->palette);
                img->palette = palette;
                img->paletteSlots = maxSlots;
                img->ownsPalette = false;
            }

            if (finalCount + 1 > finalCapacity) {
                finalCapacity *= 2;
                finalGroups = safeRealloc(finalGroups, finalCapacity * sizeof(ClutGroup));
            }
            ClutGroup* g = &finalGroups[finalCount];
            g->id = (int32_t) finalCount;
            g->bpp = m->bpp;
            g->colors = m->colors;       // transfer ownership
            g->colorCount = m->colorCount;
            g->palette = palette;
            g->imageIndices = m->imageIndices; // transfer ownership
            g->imageIndexCount = m->imageIndexCount;
            g->imageIndexCapacity = m->imageIndexCapacity;
            finalCount++;
        }
    }
    free(groups4);
    free(groups8);

    // All ClutImages now point at the final palettes; intermediate merge palettes can be freed
    PaletteLeakList_free(&leaks);

    // initialGroups' palettes are now safe to free (every ClutImage has been repointed at a final palette above)
    repeat(initialGroupCount, i) ClutGroup_free(&initialGroups[i]);
    free(initialGroups);

    *outGroupCount = finalCount;
    return finalGroups;
}
