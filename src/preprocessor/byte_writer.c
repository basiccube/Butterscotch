#include "byte_writer.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

ByteWriter ByteWriter_create(size_t initialCapacity) {
    if (256 > initialCapacity) initialCapacity = 256;
    return (ByteWriter){.buffer = safeMalloc(initialCapacity), .size = 0, .capacity = initialCapacity};
}

void ByteWriter_free(ByteWriter* writer) {
    free(writer->buffer);
    writer->buffer = nullptr;
    writer->size = 0;
    writer->capacity = 0;
}

void ByteWriter_ensureCapacity(ByteWriter* writer, size_t additionalBytes) {
    size_t required = writer->size + additionalBytes;
    if (writer->capacity >= required) return;

    size_t newCapacity = writer->capacity;
    while (required > newCapacity) {
        newCapacity = (size_t)((double) newCapacity * 1.5);
        if (newCapacity <= writer->capacity) newCapacity = required;
    }
    writer->buffer = safeRealloc(writer->buffer, newCapacity);
    writer->capacity = newCapacity;
}

void ByteWriter_writeUint8(ByteWriter* writer, uint8_t value) {
    ByteWriter_ensureCapacity(writer, 1);
    writer->buffer[writer->size++] = value;
}

void ByteWriter_writeUint16(ByteWriter* writer, uint16_t value) {
    ByteWriter_ensureCapacity(writer, 2);
    writer->buffer[writer->size++] = (uint8_t)(value & 0xFF);
    writer->buffer[writer->size++] = (uint8_t)((value >> 8) & 0xFF);
}

void ByteWriter_writeUint32(ByteWriter* writer, uint32_t value) {
    ByteWriter_ensureCapacity(writer, 4);
    writer->buffer[writer->size++] = (uint8_t)(value & 0xFF);
    writer->buffer[writer->size++] = (uint8_t)((value >> 8) & 0xFF);
    writer->buffer[writer->size++] = (uint8_t)((value >> 16) & 0xFF);
    writer->buffer[writer->size++] = (uint8_t)((value >> 24) & 0xFF);
}

void ByteWriter_writeBytes(ByteWriter* writer, const uint8_t* data, size_t len) {
    if (len == 0) return;
    ByteWriter_ensureCapacity(writer, len);
    memcpy(writer->buffer + writer->size, data, len);
    writer->size += len;
}

void ByteWriter_writeZeroPadding(ByteWriter* writer, size_t count) {
    if (count == 0) return;
    ByteWriter_ensureCapacity(writer, count);
    memset(writer->buffer + writer->size, 0, count);
    writer->size += count;
}

uint8_t* ByteWriter_detach(ByteWriter* writer, size_t* outSize) {
    uint8_t* buf = writer->buffer;
    *outSize = writer->size;
    writer->buffer = nullptr;
    writer->size = 0;
    writer->capacity = 0;
    return buf;
}
