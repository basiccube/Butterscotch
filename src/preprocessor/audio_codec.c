#include "audio_codec.h"
#include "utils.h"

#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PUSHDATA_API 1
#include "stb_vorbis.c"

#include <stdlib.h>
#include <string.h>
#include <math.h>

void AudioData_free(AudioData* audio) {
    free(audio->data);
    audio->data = nullptr;
    audio->dataSize = 0;
    audio->valid = false;
}

// ===[ IMA ADPCM Encoder ]===

static const int IMA_STEP_TABLE[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int IMA_INDEX_TABLE[] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int clampInt(int x, int lo, int hi) {
    if (lo > x) return lo;
    if (x > hi) return hi;
    return x;
}

static inline int16_t clampInt16(int x) {
    if (-32768 > x) return -32768;
    if (x > 32767) return 32767;
    return (int16_t) x;
}

static uint8_t* imaAdpcmEncode(const int16_t* pcmSamples, size_t totalSamples, int channels, size_t* outSize) {
    // Each sample becomes one 4-bit nibble, packed two per byte
    size_t adpcmSize = (totalSamples + 1) / 2;
    uint8_t* output = safeCalloc(adpcmSize == 0 ? 1 : adpcmSize, 1);

    // Per-channel encoder state
    int* predictor = safeCalloc(channels, sizeof(int));
    int* stepIndex = safeCalloc(channels, sizeof(int));

    size_t outPos = 0;
    bool nibbleHigh = false;
    repeat(totalSamples, i) {
        int ch = (int)(i % (size_t) channels);
        int sample = pcmSamples[i];
        int step = IMA_STEP_TABLE[stepIndex[ch]];

        int diff = sample - predictor[ch];
        int nibble = 0;
        if (0 > diff) {
            nibble = 8;
            diff = -diff;
        }
        if (diff >= step)     { nibble |= 4; diff -= step; }
        if (diff >= step / 2) { nibble |= 2; diff -= step / 2; }
        if (diff >= step / 4) { nibble |= 1; }

        // Decode the nibble to update predictor (same as decoder would)
        int decodedDiff = step >> 3;
        if (nibble & 4) decodedDiff += step;
        if (nibble & 2) decodedDiff += step >> 1;
        if (nibble & 1) decodedDiff += step >> 2;
        if (nibble & 8) decodedDiff = -decodedDiff;

        predictor[ch] = clampInt(predictor[ch] + decodedDiff, -32768, 32767);
        stepIndex[ch] = clampInt(stepIndex[ch] + IMA_INDEX_TABLE[nibble], 0, 88);

        // Pack nibbles: low nibble first, then high nibble
        if (!nibbleHigh) {
            output[outPos] = (uint8_t)(nibble & 0x0F);
            nibbleHigh = true;
        } else {
            output[outPos] = (uint8_t)(output[outPos] | ((nibble & 0x0F) << 4));
            outPos++;
            nibbleHigh = false;
        }
    }

    free(predictor);
    free(stepIndex);
    *outSize = adpcmSize;
    return output;
}

// ===[ Resampling and downmixing ]===

// Downsample interleaved PCM samples to 22050 Hz using linear interpolation.
// Returns the original samples unchanged if the sample rate is already 22050 Hz or below.
static int16_t* downsampleTo22050(int16_t* samples, size_t sampleCount, int sampleRate, int channels, size_t* outSampleCount, int* outRate) {
    int targetRate = 22050;
    if (targetRate >= sampleRate) {
        *outSampleCount = sampleCount;
        *outRate = sampleRate;
        return samples;
    }

    size_t frameCount = sampleCount / (size_t) channels;
    double ratio = (double) sampleRate / (double) targetRate;
    size_t newFrameCount = (size_t)((double) frameCount / ratio);
    int16_t* output = safeMalloc(newFrameCount * (size_t) channels * sizeof(int16_t));

    repeat(newFrameCount, f) {
        double srcPos = (double) f * ratio;
        size_t srcIdx = (size_t) srcPos;
        float frac = (float)(srcPos - (double) srcIdx);

        repeat(channels, ch) {
            int s0 = (int) samples[srcIdx * (size_t) channels + (size_t) ch];
            int s1 = (frameCount > srcIdx + 1) ? (int) samples[(srcIdx + 1) * (size_t) channels + (size_t) ch] : s0;
            int v = (int)((float) s0 + (float)(s1 - s0) * frac);
            output[f * (size_t) channels + (size_t) ch] = clampInt16(v);
        }
    }

    free(samples);
    *outSampleCount = newFrameCount * (size_t) channels;
    *outRate = targetRate;
    return output;
}

// Downmix interleaved multi-channel PCM samples to mono by averaging all channels.
// Returns the original samples unchanged if already mono.
static int16_t* downmixToMono(int16_t* samples, size_t sampleCount, int channels, size_t* outSampleCount) {
    if (1 >= channels) {
        *outSampleCount = sampleCount;
        return samples;
    }

    size_t frameCount = sampleCount / (size_t) channels;
    int16_t* output = safeMalloc(frameCount * sizeof(int16_t));
    repeat(frameCount, f) {
        int sum = 0;
        repeat(channels, ch) {
            sum += (int) samples[f * (size_t) channels + (size_t) ch];
        }
        output[f] = clampInt16(sum / channels);
    }
    free(samples);
    *outSampleCount = frameCount;
    return output;
}

// ===[ WAV parser ]===

static int readShortLE(const uint8_t* data, size_t offset) {
    return (int) data[offset] | ((int) data[offset + 1] << 8);
}

static int readIntLE(const uint8_t* data, size_t offset) {
    return (int) data[offset] |
           ((int) data[offset + 1] << 8) |
           ((int) data[offset + 2] << 16) |
           ((int) data[offset + 3] << 24);
}

// Parse a WAV file and extract raw PCM data. Returns invalid for non-WAV (e.g. OGG) or non-PCM formats.
static AudioData parseWav(const uint8_t* data, size_t size) {
    AudioData invalid = {.valid = false};
    if (4 > size) return invalid;

    // Check RIFF magic
    if (data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') return invalid;

    // Check WAVE format at offset 8
    if (12 > size) return invalid;
    if (data[8] != 'W' || data[9] != 'A' || data[10] != 'V' || data[11] != 'E') return invalid;

    size_t pos = 12;
    int sampleRate = 0, channels = 0, bitsPerSample = 0;
    uint8_t* pcmData = nullptr;
    size_t pcmDataSize = 0;
    bool foundFmt = false;

    // Walk through chunks
    while (size >= pos + 8) {
        char chunkId[4] = { (char) data[pos], (char) data[pos + 1], (char) data[pos + 2], (char) data[pos + 3] };
        int chunkSize = readIntLE(data, pos + 4);
        pos += 8;

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            if (pos + 16 > size) { free(pcmData); return invalid; }
            int audioFormat = readShortLE(data, pos);
            // Only PCM (format 1) is supported
            if (audioFormat != 1) { free(pcmData); return invalid; }
            channels = readShortLE(data, pos + 2);
            sampleRate = readIntLE(data, pos + 4);
            bitsPerSample = readShortLE(data, pos + 14);
            foundFmt = true;
        } else if (memcmp(chunkId, "data", 4) == 0) {
            size_t dataEnd = (pos + (size_t) chunkSize > size) ? size : pos + (size_t) chunkSize;
            pcmDataSize = dataEnd - pos;
            pcmData = safeMalloc(pcmDataSize);
            memcpy(pcmData, data + pos, pcmDataSize);
        }

        pos += (size_t) chunkSize;
        // Chunks are word-aligned
        if (pos % 2 != 0) pos++;
    }

    if (!foundFmt || pcmData == nullptr) { free(pcmData); return invalid; }

    // Convert PCM data to 16-bit samples, then encode as IMA ADPCM
    int16_t* pcmSamples;
    size_t pcmSampleCount;
    if (bitsPerSample == 16) {
        // Already 16-bit LE, just reinterpret
        pcmSampleCount = pcmDataSize / 2;
        pcmSamples = safeMalloc(pcmSampleCount * sizeof(int16_t));
        repeat(pcmSampleCount, i) {
            pcmSamples[i] = (int16_t)((int) pcmData[i * 2] | ((int) pcmData[i * 2 + 1] << 8));
        }
    } else if (bitsPerSample == 8) {
        // 8-bit unsigned PCM, convert to 16-bit signed
        pcmSampleCount = pcmDataSize;
        pcmSamples = safeMalloc(pcmSampleCount * sizeof(int16_t));
        repeat(pcmSampleCount, i) {
            int v = ((int) pcmData[i] - 128) * 257;
            pcmSamples[i] = (int16_t) v;
        }
    } else {
        free(pcmData);
        return invalid;
    }
    free(pcmData);

    // Downsample to 22050 Hz if the sample rate is higher
    int finalRate;
    pcmSamples = downsampleTo22050(pcmSamples, pcmSampleCount, sampleRate, channels, &pcmSampleCount, &finalRate);

    // Downmix to mono
    pcmSamples = downmixToMono(pcmSamples, pcmSampleCount, channels, &pcmSampleCount);

    size_t adpcmSize;
    uint8_t* adpcmData = imaAdpcmEncode(pcmSamples, pcmSampleCount, 1, &adpcmSize);
    free(pcmSamples);

    return (AudioData){
        .format = AUDIO_FORMAT_ADPCM,
        .sampleRate = finalRate,
        .channels = 1,
        .bitsPerSample = 4,
        .data = adpcmData,
        .dataSize = adpcmSize,
        .sampleCount = (int) pcmSampleCount,
        .valid = true
    };
}

// ===[ OGG parser ]===

// Decode an OGG Vorbis file and encode to IMA ADPCM. Returns invalid if the data is not a valid OGG Vorbis file.
static AudioData parseOgg(const uint8_t* data, size_t size) {
    AudioData invalid = {.valid = false};
    int err = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int) size, &err, nullptr);
    if (vorbis == nullptr) return invalid;

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    unsigned int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
    if (totalSamples == 0) {
        stb_vorbis_close(vorbis);
        return invalid;
    }

    // Decode all samples as interleaved floats
    size_t totalFloats = (size_t) totalSamples * (size_t) info.channels;
    float* floatBuf = safeMalloc(totalFloats * sizeof(float));
    int decoded = stb_vorbis_get_samples_float_interleaved(vorbis, info.channels, floatBuf, (int) totalFloats);
    stb_vorbis_close(vorbis);
    if (decoded == 0) {
        free(floatBuf);
        return invalid;
    }

    // Convert float samples to 16-bit signed PCM (interleaved)
    size_t pcmSampleCount = (size_t) decoded * (size_t) info.channels;
    int16_t* pcmSamples = safeMalloc(pcmSampleCount * sizeof(int16_t));
    repeat(pcmSampleCount, i) {
        float clamped = floatBuf[i];
        if (-1.0f > clamped) clamped = -1.0f;
        else if (clamped > 1.0f) clamped = 1.0f;
        pcmSamples[i] = clampInt16((int)(clamped * 32767.0f));
    }
    free(floatBuf);

    // Downsample to 22050 Hz if the sample rate is higher
    int finalRate;
    pcmSamples = downsampleTo22050(pcmSamples, pcmSampleCount, (int) info.sample_rate, info.channels, &pcmSampleCount, &finalRate);

    // Downmix to mono
    pcmSamples = downmixToMono(pcmSamples, pcmSampleCount, info.channels, &pcmSampleCount);

    // Encode to IMA ADPCM
    size_t adpcmSize;
    uint8_t* adpcmData = imaAdpcmEncode(pcmSamples, pcmSampleCount, 1, &adpcmSize);
    free(pcmSamples);

    return (AudioData){
        .format = AUDIO_FORMAT_ADPCM,
        .sampleRate = finalRate,
        .channels = 1,
        .bitsPerSample = 4,
        .data = adpcmData,
        .dataSize = adpcmSize,
        .sampleCount = (int) pcmSampleCount,
        .valid = true
    };
}

AudioData AudioCodec_decode(const uint8_t* bytes, size_t size) {
    AudioData wav = parseWav(bytes, size);
    if (wav.valid) return wav;
    return parseOgg(bytes, size);
}
