#pragma once

#include "common.h"
#include <stdint.h>
#include <stddef.h>

#define AUDIO_FORMAT_PCM 0
#define AUDIO_FORMAT_ADPCM 1

typedef struct {
    int format;          // AUDIO_FORMAT_PCM or AUDIO_FORMAT_ADPCM
    int sampleRate;
    int channels;
    int bitsPerSample;   // Only meaningful for PCM (8 or 16)
    uint8_t* data;       // Raw PCM, raw OGG, or ADPCM encoded data (owned)
    size_t dataSize;
    int sampleCount;     // Decoded sample count per channel; length in seconds = sampleCount / sampleRate
    bool valid;          // true on a successful decode; false if the input wasn't recognized
} AudioData;

// Decode any supported audio file (WAV PCM or OGG Vorbis), downsample to <=22050 Hz, downmix to mono, and encode to IMA ADPCM. The returned AudioData owns its data buffer; caller must AudioData_free it.
AudioData AudioCodec_decode(const uint8_t* bytes, size_t size);

void AudioData_free(AudioData* audio);
