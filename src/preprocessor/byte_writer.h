#pragma once

#include "common.h"
#include <stdint.h>
#include <stddef.h>

// Growable little-endian byte writer. Owns a heap buffer that grows as values are appended.
typedef struct {
    uint8_t* buffer;
    size_t size;
    size_t capacity;
} ByteWriter;

// Creates a ByteWriter with the given initial capacity (clamped to at least 256 bytes).
ByteWriter ByteWriter_create(size_t initialCapacity);

// Frees the buffer. After this call the ByteWriter must not be used.
void ByteWriter_free(ByteWriter* writer);

// Ensures the buffer can hold at least `additionalBytes` more content without reallocating. Grows by 1.5x until it fits.
void ByteWriter_ensureCapacity(ByteWriter* writer, size_t additionalBytes);

void ByteWriter_writeUint8(ByteWriter* writer, uint8_t value);
void ByteWriter_writeUint16(ByteWriter* writer, uint16_t value);
void ByteWriter_writeUint32(ByteWriter* writer, uint32_t value);

// Appends "len" bytes from "data".
void ByteWriter_writeBytes(ByteWriter* writer, const uint8_t* data, size_t len);

// Appends "count" zero bytes.
void ByteWriter_writeZeroPadding(ByteWriter* writer, size_t count);

// Detaches the buffer: ownership transfers to the caller and the writer becomes empty. Returns nullptr if size is 0.
uint8_t* ByteWriter_detach(ByteWriter* writer, size_t* outSize);
