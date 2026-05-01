#include "median_cut.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t* pixels;
    size_t pixelCount;
    int rMin, rMax, gMin, gMax, bMin, bMax;
    bool unsplittable;
    bool ownsPixels; // true if this box owns its pixels array (allocated via malloc)
} Box;

static void Box_init(Box* box, uint32_t* pixels, size_t pixelCount, bool ownsPixels) {
    box->pixels = pixels;
    box->pixelCount = pixelCount;
    box->rMin = 255; box->rMax = 0;
    box->gMin = 255; box->gMax = 0;
    box->bMin = 255; box->bMax = 0;
    box->unsplittable = false;
    box->ownsPixels = ownsPixels;
    repeat(pixelCount, i) {
        uint32_t p = pixels[i];
        int r = (int) ((p >> 16) & 0xFF);
        int g = (int) ((p >> 8) & 0xFF);
        int b = (int) (p & 0xFF);
        if (box->rMin > r) box->rMin = r;
        if (r > box->rMax) box->rMax = r;
        if (box->gMin > g) box->gMin = g;
        if (g > box->gMax) box->gMax = g;
        if (box->bMin > b) box->bMin = b;
        if (b > box->bMax) box->bMax = b;
    }
}

static int Box_rRange(const Box* b) { return b->rMax - b->rMin; }
static int Box_gRange(const Box* b) { return b->gMax - b->gMin; }
static int Box_bRange(const Box* b) { return b->bMax - b->bMin; }

static int Box_longestRange(const Box* b) {
    if (b->unsplittable) return 0;
    int rr = Box_rRange(b), gr = Box_gRange(b), br = Box_bRange(b);
    int m = rr;
    if (gr > m) m = gr;
    if (br > m) m = br;
    return m;
}

static uint32_t Box_average(const Box* b) {
    int64_t rSum = 0, gSum = 0, bSum = 0;
    repeat(b->pixelCount, i) {
        uint32_t p = b->pixels[i];
        rSum += (int64_t) ((p >> 16) & 0xFF);
        gSum += (int64_t) ((p >> 8) & 0xFF);
        bSum += (int64_t) (p & 0xFF);
    }
    size_t n = b->pixelCount;
    uint32_t r = (uint32_t)(rSum / (int64_t) n);
    uint32_t g = (uint32_t)(gSum / (int64_t) n);
    uint32_t b_ = (uint32_t)(bSum / (int64_t) n);
    return ((uint32_t) 0xFF << 24) | (r << 16) | (g << 8) | b_;
}

// Sort key extractors for the three channels.
static int keyR(uint32_t p) { return (int) ((p >> 16) & 0xFF); }
static int keyG(uint32_t p) { return (int) ((p >> 8) & 0xFF); }
static int keyB(uint32_t p) { return (int) (p & 0xFF); }

typedef int (*KeyFn)(uint32_t);
static KeyFn g_sortKey;

static int sortByKey(const void* a, const void* b) {
    int ka = g_sortKey(*(const uint32_t*) a);
    int kb = g_sortKey(*(const uint32_t*) b);
    return ka - kb;
}

// Split at a real value boundary on the widest channel. Returns true on success and writes the two children to outLeft / outRight.
// Returns false when every pixel in the box has the same value on that channel (no real split exists).
static bool Box_split(Box* box, Box* outLeft, Box* outRight) {
    int rRange = Box_rRange(box);
    int gRange = Box_gRange(box);
    int bRange = Box_bRange(box);
    KeyFn keyOf;
    if (rRange >= gRange && rRange >= bRange) keyOf = keyR;
    else if (gRange >= bRange) keyOf = keyG;
    else keyOf = keyB;

    // Sort a copy of the pixels by the chosen channel
    uint32_t* sorted = safeMalloc(box->pixelCount * sizeof(uint32_t));
    memcpy(sorted, box->pixels, box->pixelCount * sizeof(uint32_t));
    g_sortKey = keyOf;
    qsort(sorted, box->pixelCount, sizeof(uint32_t), sortByKey);

    size_t targetIdx = box->pixelCount / 2;
    int targetKey = keyOf(sorted[targetIdx]);

    // Walk outward from targetIdx until the key changes, so the split lands between
    // distinct color values rather than slicing a run of identical pixels in half.
    size_t fwd = targetIdx;
    while (box->pixelCount > fwd && keyOf(sorted[fwd]) == targetKey) fwd++;
    size_t bwd = targetIdx;
    while (bwd > 0 && keyOf(sorted[bwd - 1]) == targetKey) bwd--;

    size_t splitIdx;
    if (bwd > 0 && box->pixelCount > fwd) {
        splitIdx = (targetIdx - bwd <= fwd - targetIdx) ? bwd : fwd;
    } else if (bwd > 0) {
        splitIdx = bwd;
    } else if (box->pixelCount > fwd) {
        splitIdx = fwd;
    } else {
        free(sorted);
        return false;
    }

    size_t leftCount = splitIdx;
    size_t rightCount = box->pixelCount - splitIdx;
    uint32_t* leftPixels = safeMalloc(leftCount * sizeof(uint32_t));
    uint32_t* rightPixels = safeMalloc(rightCount * sizeof(uint32_t));
    memcpy(leftPixels, sorted, leftCount * sizeof(uint32_t));
    memcpy(rightPixels, sorted + leftCount, rightCount * sizeof(uint32_t));
    free(sorted);

    Box_init(outLeft, leftPixels, leftCount, true);
    Box_init(outRight, rightPixels, rightCount, true);
    return true;
}

static void Box_free(Box* box) {
    if (box->ownsPixels) free(box->pixels);
    box->pixels = nullptr;
    box->pixelCount = 0;
}

size_t MedianCut_quantize(const uint32_t* pixels, size_t pixelCount, size_t maxColors, uint32_t* outPalette) {
    if (pixelCount == 0 || maxColors == 0) return 0;

    // We don't own the input pixels, so for the initial box we copy them so all boxes uniformly own their storage
    uint32_t* initialPixels = safeMalloc(pixelCount * sizeof(uint32_t));
    memcpy(initialPixels, pixels, pixelCount * sizeof(uint32_t));

    size_t boxesCapacity = 16;
    Box* boxes = safeMalloc(boxesCapacity * sizeof(Box));
    size_t boxCount = 0;
    Box_init(&boxes[boxCount++], initialPixels, pixelCount, true);

    while (maxColors > boxCount) {
        // Select the box that contributes the most quantization error: pixelCount * longestRange.
        // Pure range-based selection over-splits dominant colors; pixelCount weights toward
        // boxes that lots of output pixels will draw from.
        ptrdiff_t targetIdx = -1;
        int64_t bestScore = 0;
        repeat(boxCount, i) {
            Box* b = &boxes[i];
            int lr = Box_longestRange(b);
            if (2 > b->pixelCount || lr == 0) continue;
            int64_t score = (int64_t) b->pixelCount * (int64_t) lr;
            if (score > bestScore) {
                bestScore = score;
                targetIdx = (ptrdiff_t) i;
            }
        }
        if (0 > targetIdx) break;

        Box left, right;
        Box* target = &boxes[targetIdx];
        if (!Box_split(target, &left, &right)) {
            // Mark this box as unsplittable so we stop trying it
            target->unsplittable = true;
            continue;
        }
        Box_free(target);

        if (boxCount + 1 > boxesCapacity) {
            boxesCapacity *= 2;
            boxes = safeRealloc(boxes, boxesCapacity * sizeof(Box));
        }

        // Match Kotlin: boxes.remove(target); boxes.add(left); boxes.add(right). Shift remaining left, then append both at the end.
        for (size_t i = (size_t) targetIdx; i < boxCount - 1; i++) {
            boxes[i] = boxes[i + 1];
        }
        boxCount--;
        boxes[boxCount++] = left;
        boxes[boxCount++] = right;
    }

    repeat(boxCount, i) {
        outPalette[i] = Box_average(&boxes[i]);
        Box_free(&boxes[i]);
    }
    size_t result = boxCount;
    free(boxes);
    return result;
}
