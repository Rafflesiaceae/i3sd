#include "i3sd/output.h"

#include <errno.h>
#include <string.h>

void i3sd_output_init(struct i3sd_output *output) {
    *output = (struct i3sd_output){.first_frame = true};
}

void i3sd_output_destroy(struct i3sd_output *output) {
    i3sd_buffer_destroy(&output->last_completed);
    i3sd_buffer_destroy(&output->current);
    *output = (struct i3sd_output){0};
}

bool i3sd_output_set_frame(struct i3sd_output *output, const char *json,
                           size_t len) {
    if (len > I3SD_FRAME_MAX_BYTES) {
        return false;
    }
    if (output->offset != 0 || output->prefix_written) {
        output->render_dirty = true;
        return true;
    }

    i3sd_buffer_clear(&output->current);
    if (!i3sd_buffer_append(&output->current, json, len,
                            I3SD_FRAME_MAX_BYTES)) {
        return false;
    }

    if (output->last_completed.len == output->current.len &&
        memcmp(output->last_completed.data, output->current.data,
               output->current.len) == 0) {
        i3sd_buffer_clear(&output->current);
    }
    output->render_dirty = false;
    return true;
}

enum i3sd_flush_result i3sd_output_flush(struct i3sd_output *output,
                                         i3sd_write_fn write_fn, void *userdata,
                                         size_t byte_budget,
                                         int *error_number) {
    if (!output->first_frame && !output->prefix_written &&
        output->current.len != 0 && byte_budget > 0) {
        const ssize_t written = write_fn(userdata, ",", 1);
        if (written == 1) {
            output->prefix_written = true;
            byte_budget--;
        } else if (written == 0 || (written < 0 && errno == EPIPE)) {
            return I3SD_FLUSH_CLOSED;
        } else if (written < 0 && errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
            if (error_number != NULL) {
                *error_number = errno;
            }
            return I3SD_FLUSH_ERROR;
        } else {
            return I3SD_FLUSH_BLOCKED;
        }
    }
    while (output->offset < output->current.len && byte_budget > 0) {
        size_t available = output->current.len - output->offset;
        if (available > byte_budget) {
            available = byte_budget;
        }
        const ssize_t written = write_fn(
            userdata, output->current.data + output->offset, available);
        if (written > 0) {
            output->offset += (size_t)written;
            byte_budget -= (size_t)written;
            continue;
        }
        if (written == 0) {
            return I3SD_FLUSH_CLOSED;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return I3SD_FLUSH_BLOCKED;
        }
        if (errno == EPIPE) {
            return I3SD_FLUSH_CLOSED;
        }
        if (error_number != NULL) {
            *error_number = errno;
        }
        return I3SD_FLUSH_ERROR;
    }

    if (output->offset == output->current.len && output->current.len != 0) {
        struct i3sd_buffer temporary = output->last_completed;
        output->last_completed = output->current;
        output->current = temporary;
        i3sd_buffer_clear(&output->current);
        output->offset = 0;
        output->first_frame = false;
        output->prefix_written = false;
    }
    return output->current.len == 0 ? I3SD_FLUSH_DRAINED : I3SD_FLUSH_BLOCKED;
}

bool i3sd_output_has_pending(const struct i3sd_output *output) {
    return output->offset < output->current.len;
}
