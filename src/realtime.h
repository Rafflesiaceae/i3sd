#ifndef I3SD_REALTIME_H
#define I3SD_REALTIME_H

#include "runtime.h"

#include <stdint.h>

/* Current CLOCK_REALTIME in nanoseconds since the Unix epoch. */
uint64_t i3sd_realtime_now_ns(void);

/* Create, arm or close the shared CLOCK_REALTIME timerfd so that it wakes
 * for the earliest realtime deadline and reports clock discontinuities
 * while realtime timers or clock observers exist. */
void i3sd_realtime_reconcile(struct app *app);

/* Drain the timerfd, notify clock observers of a discontinuity and run due
 * realtime timers of the current generation. */
void i3sd_realtime_process(struct app *app);

/* Close the timerfd during shutdown. */
void i3sd_realtime_close(struct app *app);

#endif
