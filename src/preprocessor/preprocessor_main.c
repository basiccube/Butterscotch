#include "pipeline.h"
#include "utils.h"

#include "stb_ds.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool endsWithIgnoreCase(const char* s, const char* suffix) {
    size_t sl = strlen(s);
    size_t fl = strlen(suffix);
    if (sl < fl) return false;
    return strcasecmp(s + sl - fl, suffix) == 0;
}

static uint8_t* readWholeFile(const char* path, size_t* outSize) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = safeMalloc((size_t) size);
    if (fread(buf, 1, (size_t) size, f) != (size_t) size) {
        fprintf(stderr, "Failed to read %s\n", path);
        fclose(f);
        free(buf);
        return nullptr;
    }
    fclose(f);
    *outSize = (size_t) size;
    return buf;
}

static bool writeWholeFile(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
        return false;
    }
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to write %s\n", path);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool isDirectory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool ensureDirectory(const char* path) {
    if (isDirectory(path)) return true;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

// Match "audiogroup<N>.dat" (case-insensitive). Returns the group id, or -1 if no match. Group 0 is excluded since it's embedded in data.win.
static int matchAudiogroupFile(const char* fname) {
    if (strncasecmp(fname, "audiogroup", 10) != 0) return -1;
    const char* p = fname + 10;
    if (!isdigit((unsigned char) *p)) return -1;
    int id = 0;
    while (isdigit((unsigned char) *p)) { id = id * 10 + (*p - '0'); p++; }
    if (strcasecmp(p, ".dat") != 0) return -1;
    return id;
}

// Scan a directory for audiogroup%d.dat files. Audiogroup 0 is embedded in data.win, so we skip it.
static void loadAudioGroupFiles(const char* dir, ExternalFile** outFiles, size_t* outCount) {
    DIR* d = opendir(dir);
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        int groupId = matchAudiogroupFile(e->d_name);
        if (0 > groupId || groupId == 0) continue; // audiogroup 0 is in data.win
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        ExternalFile f = {.fileName = safeStrdup(path), .bytes = nullptr, .size = 0};
        // For audiogroup files we don't load bytes here; the pipeline DataWin_parse opens them by path
        arrput(*outFiles, f);
    }
    closedir(d);
    *outCount = arrlen(*outFiles);
    if (*outCount > 0) {
        printf("Found %zu audiogroup files in %s\n", *outCount, dir);
    }
}

// Scan a directory for top-level audio files (OGG, WAV) keyed by basename.
static void loadExternalAudioFiles(const char* dir, ExternalFile** outFiles, size_t* outCount) {
    DIR* d = opendir(dir);
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!endsWithIgnoreCase(e->d_name, ".ogg") && !endsWithIgnoreCase(e->d_name, ".wav")) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t size;
        uint8_t* bytes = readWholeFile(path, &size);
        if (bytes == nullptr) continue;
        ExternalFile f = {.fileName = safeStrdup(e->d_name), .bytes = bytes, .size = size};
        arrput(*outFiles, f);
    }
    closedir(d);
    *outCount = arrlen(*outFiles);
    if (*outCount > 0) {
        printf("Found %zu external audio files in %s\n", *outCount, dir);
    }
}

// Recursively walk one subdirectory (e.g. mus/) and gather .ogg files keyed by relative path from `baseDir`.
static void walkOggSubdir(const char* baseDir, const char* subdirPath, const char* relativePrefix, ExternalFile** outFiles) {
    DIR* d = opendir(subdirPath);
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", subdirPath, e->d_name);
        char relPath[1024];
        if (relativePrefix[0] != '\0') {
            snprintf(relPath, sizeof(relPath), "%s/%s", relativePrefix, e->d_name);
        } else {
            snprintf(relPath, sizeof(relPath), "%s", e->d_name);
        }
        if (isDirectory(fullPath)) {
            walkOggSubdir(baseDir, fullPath, relPath, outFiles);
        } else if (endsWithIgnoreCase(e->d_name, ".ogg")) {
            size_t size;
            uint8_t* bytes = readWholeFile(fullPath, &size);
            if (bytes == nullptr) continue;
            ExternalFile f = {.fileName = safeStrdup(relPath), .bytes = bytes, .size = size};
            arrput(*outFiles, f);
        }
    }
    closedir(d);
}

static int compareExternalFileByName(const void* a, const void* b) {
    return strcmp(((const ExternalFile*) a)->fileName, ((const ExternalFile*) b)->fileName);
}

// Scan for subdirectories containing OGG files (e.g. mus/) and load them keyed by relative path. Results are sorted by relative path to match Kotlin's `entries.sortedBy { it.key }`.
static void loadMusFiles(const char* dir, ExternalFile** outFiles, size_t* outCount) {
    DIR* d = opendir(dir);
    if (d == nullptr) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!isDirectory(path)) continue;
        walkOggSubdir(dir, path, e->d_name, outFiles);
    }
    closedir(d);
    qsort(*outFiles, arrlen(*outFiles), sizeof(ExternalFile), compareExternalFileByName);
    *outCount = arrlen(*outFiles);
    if (*outCount > 0) {
        printf("Found %zu streamed music files in subdirectories of %s\n", *outCount, dir);
    }
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options] <data.win path>\n"
        "Options:\n"
        "  -o, --output <dir>      Output directory (default: output)\n"
        "  --force-4bpp <regex>    Force images matching this POSIX regex to 4bpp. Repeatable.\n"
        "  -h, --help              Show this help.\n",
        prog);
}

int main(int argc, char** argv) {
    const char* outputDir = "output";
    const char* dataWinPath = nullptr;
    char** force4bppPatterns = nullptr;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            outputDir = argv[++i];
            continue;
        }
        if (strcmp(a, "--force-4bpp") == 0) {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            arrput(force4bppPatterns, argv[++i]);
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
            return 1;
        }
        if (dataWinPath != nullptr) {
            fprintf(stderr, "Multiple positional arguments not supported\n");
            usage(argv[0]);
            return 1;
        }
        dataWinPath = a;
    }
    if (dataWinPath == nullptr) {
        usage(argv[0]);
        return 1;
    }

    if (!ensureDirectory(outputDir)) return 1;

    // Determine the directory containing data.win for sibling-file lookups
    char dataDir[1024];
    snprintf(dataDir, sizeof(dataDir), "%s", dataWinPath);
    char* lastSlash = strrchr(dataDir, '/');
    if (lastSlash != nullptr) *lastSlash = '\0';
    else snprintf(dataDir, sizeof(dataDir), ".");

    printf("Parsing %s...\n", dataWinPath);

    ExternalFile* externalAudio = nullptr;
    size_t externalAudioCount = 0;
    loadExternalAudioFiles(dataDir, &externalAudio, &externalAudioCount);

    ExternalFile* audioGroups = nullptr;
    size_t audioGroupCount = 0;
    loadAudioGroupFiles(dataDir, &audioGroups, &audioGroupCount);

    ExternalFile* musFiles = nullptr;
    size_t musFileCount = 0;
    loadMusFiles(dataDir, &musFiles, &musFileCount);

    ProcessingOptions opts = {
        .dataWinPath = dataWinPath,
        .externalAudioFiles = externalAudio,
        .externalAudioFileCount = externalAudioCount,
        .audioGroupFiles = audioGroups,
        .audioGroupFileCount = audioGroupCount,
        .musFiles = musFiles,
        .musFileCount = musFileCount,
        .force4bppPatterns = force4bppPatterns,
        .force4bppPatternCount = (size_t) arrlen(force4bppPatterns),
    };

    ProcessingResult result = Pipeline_processDataWin(&opts);

    char outPath[1024];
    #define WRITE_BIN(NAME, BUF, SIZE) do { \
        snprintf(outPath, sizeof(outPath), "%s/" NAME, outputDir); \
        if (!writeWholeFile(outPath, BUF, SIZE)) { return 1; } \
    } while (0)
    WRITE_BIN("CLUT4.BIN",    result.clut4Bin,    result.clut4Size);
    WRITE_BIN("CLUT8.BIN",    result.clut8Bin,    result.clut8Size);
    WRITE_BIN("TEXTURES.BIN", result.texturesBin, result.texturesSize);
    WRITE_BIN("ATLAS.BIN",    result.atlasBin,    result.atlasSize);
    WRITE_BIN("SOUNDBNK.BIN", result.soundBnkBin, result.soundBnkSize);
    WRITE_BIN("SOUNDS.BIN",   result.soundsBin,   result.soundsSize);
    #undef WRITE_BIN

    printf("\nAll files written to %s\n", outputDir);
    printf("Done!\n");

    ProcessingResult_free(&result);

    repeat(externalAudioCount, i) {
        free(externalAudio[i].fileName);
        free(externalAudio[i].bytes);
    }
    arrfree(externalAudio);
    repeat(audioGroupCount, i) {
        free(audioGroups[i].fileName);
        // bytes were not loaded
    }
    arrfree(audioGroups);
    repeat(musFileCount, i) {
        free(musFiles[i].fileName);
        free(musFiles[i].bytes);
    }
    arrfree(musFiles);
    arrfree(force4bppPatterns);

    return 0;
}
