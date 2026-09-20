#pragma once

#include "i3sd/buffer.h"

#include <stdbool.h>
#include <stddef.h>

#define I3SD_CLICK_MAX_BYTES (64U * 1024U)
#define I3SD_CLICK_MAX_DEPTH 32U

enum i3sd_click_result {
    I3SD_CLICK_NONE,
    I3SD_CLICK_EVENT,
    I3SD_CLICK_DROPPED,
    I3SD_CLICK_FATAL_FRAMING,
};

struct i3sd_click_framer {
    struct i3sd_buffer event;
    unsigned depth;
    bool outer_seen;
    bool in_event;
    bool in_string;
    bool escaped;
    bool discarding;
    bool irrecoverable;
};

void i3sd_click_framer_destroy(struct i3sd_click_framer *framer);
enum i3sd_click_result i3sd_click_framer_feed(struct i3sd_click_framer *framer,
                                              const char *data, size_t len,
                                              size_t *consumed,
                                              const char **event,
                                              size_t *event_len);
