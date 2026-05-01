#pragma once

#include "common.h"
#include "byte_writer.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char* fileName; // owned filename (relative path, e.g. "audiogroup1.dat" or "snd_foo.ogg")
    uint8_t* bytes;  // owned file contents
    size_t size;
} ExternalFile;

typedef struct {
    char* gameName;
    uint8_t* clut4Bin;
    size_t clut4Size;
    uint8_t* clut8Bin;
    size_t clut8Size;
    uint8_t* texturesBin;
    size_t texturesSize;
    uint8_t* atlasBin;
    size_t atlasSize;
    uint8_t* soundBnkBin;
    size_t soundBnkSize;
    uint8_t* soundsBin;
    size_t soundsSize;
} ProcessingResult;

typedef struct {
    const char* dataWinPath;
    ExternalFile* externalAudioFiles;
    size_t externalAudioFileCount;
    ExternalFile* audioGroupFiles; // one per audiogroup%d.dat (groupId stored in fileName "audiogroup<N>.dat")
    size_t audioGroupFileCount;
    ExternalFile* musFiles; // streamed music files keyed by relative path
    size_t musFileCount;
    char** force4bppPatterns; // POSIX regex patterns
    size_t force4bppPatternCount;
} ProcessingOptions;

ProcessingResult Pipeline_processDataWin(const ProcessingOptions* options);
void ProcessingResult_free(ProcessingResult* result);
