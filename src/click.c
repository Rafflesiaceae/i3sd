#include "i3sd/click.h"

#include <ctype.h>

void i3sd_click_framer_destroy(struct i3sd_click_framer *framer) {
    i3sd_buffer_destroy(&framer->event);
    *framer = (struct i3sd_click_framer){0};
}

static void click_reset_event(struct i3sd_click_framer *framer) {
    i3sd_buffer_clear(&framer->event);
    framer->depth = 0;
    framer->in_event = false;
    framer->in_string = false;
    framer->escaped = false;
    framer->discarding = false;
}

enum i3sd_click_result i3sd_click_framer_feed(struct i3sd_click_framer *framer,
                                              const char *data, size_t len,
                                              size_t *consumed,
                                              const char **event,
                                              size_t *event_len) {
    *consumed = 0;
    *event = NULL;
    *event_len = 0;
    if (framer->irrecoverable) {
        *consumed = len;
        return I3SD_CLICK_FATAL_FRAMING;
    }

    for (size_t index = 0; index < len; index++) {
        const char byte = data[index];
        *consumed = index + 1;

        if (!framer->outer_seen) {
            if (isspace((unsigned char)byte)) {
                continue;
            }
            if (byte != '[') {
                framer->irrecoverable = true;
                return I3SD_CLICK_FATAL_FRAMING;
            }
            framer->outer_seen = true;
            continue;
        }

        if (!framer->in_event) {
            if (isspace((unsigned char)byte) || byte == ',') {
                continue;
            }
            if (byte == ']') {
                continue;
            }
            if (byte != '{') {
                framer->irrecoverable = true;
                return I3SD_CLICK_FATAL_FRAMING;
            }
            i3sd_buffer_clear(&framer->event);
            framer->in_event = true;
            framer->depth = 1;
        }

        if (!framer->discarding &&
            !i3sd_buffer_append_char(&framer->event, byte,
                                     I3SD_CLICK_MAX_BYTES)) {
            framer->discarding = true;
        }

        if (framer->in_string) {
            if (framer->escaped) {
                framer->escaped = false;
            } else if (byte == '\\') {
                framer->escaped = true;
            } else if (byte == '"') {
                framer->in_string = false;
            }
            continue;
        }
        if (byte == '"') {
            framer->in_string = true;
            continue;
        }
        if (byte == '{' || byte == '[') {
            if (byte != '{' || framer->depth != 1 || framer->event.len != 1) {
                framer->depth++;
            }
            if (framer->depth > I3SD_CLICK_MAX_DEPTH) {
                framer->discarding = true;
            }
            continue;
        }
        if (byte != '}' && byte != ']') {
            continue;
        }
        if (framer->depth == 0) {
            framer->irrecoverable = true;
            return I3SD_CLICK_FATAL_FRAMING;
        }
        framer->depth--;
        if (framer->depth != 0) {
            continue;
        }

        if (framer->discarding) {
            click_reset_event(framer);
            return I3SD_CLICK_DROPPED;
        }
        *event = framer->event.data;
        *event_len = framer->event.len;
        framer->in_event = false;
        return I3SD_CLICK_EVENT;
    }
    return I3SD_CLICK_NONE;
}
