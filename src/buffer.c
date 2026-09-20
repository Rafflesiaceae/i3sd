#include "i3sd/buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void i3sd_buffer_destroy(struct i3sd_buffer *buffer) {
    free(buffer->data);
    *buffer = (struct i3sd_buffer){0};
}

void i3sd_buffer_clear(struct i3sd_buffer *buffer) { buffer->len = 0; }

bool i3sd_buffer_reserve(struct i3sd_buffer *buffer, size_t needed,
                         size_t limit) {
    if (needed > limit) {
        return false;
    }
    if (needed <= buffer->cap) {
        return true;
    }

    size_t next = buffer->cap == 0 ? 256 : buffer->cap;
    while (next < needed) {
        if (next > limit / 2) {
            next = limit;
            break;
        }
        next *= 2;
    }
    char *grown = realloc(buffer->data, next);
    if (grown == NULL) {
        return false;
    }
    buffer->data = grown;
    buffer->cap = next;
    return true;
}

bool i3sd_buffer_append(struct i3sd_buffer *buffer, const void *data,
                        size_t len, size_t limit) {
    if (len > SIZE_MAX - buffer->len) {
        return false;
    }
    const size_t needed = buffer->len + len;
    if (!i3sd_buffer_reserve(buffer, needed, limit)) {
        return false;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len = needed;
    return true;
}

bool i3sd_buffer_append_char(struct i3sd_buffer *buffer, char value,
                             size_t limit) {
    return i3sd_buffer_append(buffer, &value, 1, limit);
}
