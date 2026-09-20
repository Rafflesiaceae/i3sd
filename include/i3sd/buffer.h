#pragma once

#include <stdbool.h>
#include <stddef.h>

struct i3sd_buffer {
    char *data;
    size_t len;
    size_t cap;
};

void i3sd_buffer_destroy(struct i3sd_buffer *buffer);
void i3sd_buffer_clear(struct i3sd_buffer *buffer);
bool i3sd_buffer_reserve(struct i3sd_buffer *buffer, size_t needed,
                         size_t limit);
bool i3sd_buffer_append(struct i3sd_buffer *buffer, const void *data,
                        size_t len, size_t limit);
bool i3sd_buffer_append_char(struct i3sd_buffer *buffer, char value,
                             size_t limit);
