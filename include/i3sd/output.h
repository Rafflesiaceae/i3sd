#pragma once

#include "i3sd/buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define I3SD_FRAME_MAX_BYTES (2U * 1024U * 1024U)

typedef ssize_t (*i3sd_write_fn)(void *userdata, const void *data, size_t len);

enum i3sd_flush_result {
    I3SD_FLUSH_DRAINED,
    I3SD_FLUSH_BLOCKED,
    I3SD_FLUSH_CLOSED,
    I3SD_FLUSH_ERROR,
};

struct i3sd_output {
    struct i3sd_buffer last_completed;
    struct i3sd_buffer current;
    size_t offset;
    bool first_frame;
    bool prefix_written;
    bool render_dirty;
};

void i3sd_output_init(struct i3sd_output *output);
void i3sd_output_destroy(struct i3sd_output *output);
bool i3sd_output_set_frame(struct i3sd_output *output, const char *json,
                           size_t len);
enum i3sd_flush_result i3sd_output_flush(struct i3sd_output *output,
                                         i3sd_write_fn write_fn, void *userdata,
                                         size_t byte_budget, int *error_number);
bool i3sd_output_has_pending(const struct i3sd_output *output);
