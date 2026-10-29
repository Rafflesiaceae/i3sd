#include "i3sd/render_limiter.h"

#define RENDER_REFILL_NS 50000000ULL
#define RENDER_BURST 8ULL
#define RENDER_BURST_SLACK_NS ((RENDER_BURST - 1) * RENDER_REFILL_NS)

uint64_t
i3sd_render_limiter_deadline(const struct i3sd_render_limiter *limiter) {
    /* Virtual time encodes spent credits. Allow seven credits of debt before
     * the next render, so a full bucket permits eight immediate frames. */
    return limiter->virtual_time_ns > RENDER_BURST_SLACK_NS
               ? limiter->virtual_time_ns - RENDER_BURST_SLACK_NS
               : 0;
}

void i3sd_render_limiter_record(struct i3sd_render_limiter *limiter,
                                uint64_t now_ns) {
    /* Idle time refills the bucket implicitly, without division or a timer.
     * Clamping to now prevents idle time from accumulating extra credits. */
    if (limiter->virtual_time_ns < now_ns) {
        limiter->virtual_time_ns = now_ns;
    }
    limiter->virtual_time_ns += RENDER_REFILL_NS;
}
