#pragma once

#include <stdint.h>

/* Zero initialization provides a full bucket; all times are monotonic ns. */
struct i3sd_render_limiter {
    uint64_t virtual_time_ns;
};

/* Return the earliest permitted render time for comparison with current time.
 */
uint64_t
i3sd_render_limiter_deadline(const struct i3sd_render_limiter *limiter);

/* Charge one successfully constructed frame after its deadline is reached. */
void i3sd_render_limiter_record(struct i3sd_render_limiter *limiter,
                                uint64_t now_ns);
