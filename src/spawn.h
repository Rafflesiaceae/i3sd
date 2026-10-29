#ifndef I3SD_SPAWN_H
#define I3SD_SPAWN_H

#include "runtime.h"

/* Start a child in a free slot. It is killed at the monotonic deadline_ns,
 * or never when that is 0. Returns false when every slot is busy or process
 * setup fails. */
bool i3sd_spawn_open(struct app *app, struct block *block, char *const argv[],
                     const char *input, size_t input_len, size_t output_limit,
                     uint64_t deadline_ns, int callback_ref, uint64_t serial);

/* Cancel the child with this handle serial, if it is still active. */
void i3sd_spawn_cancel_serial(struct app *app, uint64_t serial);

/* Cancel every child owned by block, or by any block of generation when
 * block is NULL. Cancelled children never invoke their callback. */
void i3sd_spawn_cancel_owned(struct app *app,
                             const struct generation *generation,
                             const struct block *block);

/* Drain the output pipe registered under an epoll cookie. */
void i3sd_spawn_read(struct app *app, uint64_t cookie);

/* Record the wait status of a reaped child, if it belongs to a slot. */
void i3sd_spawn_reaped(struct app *app, pid_t pid, int wait_status);

/* Kill children past their deadline and finish deferred completions. */
void i3sd_spawn_expire(struct app *app, uint64_t now_ns);

/* Earliest monotonic time at which i3sd_spawn_expire has work to do. */
uint64_t i3sd_spawn_deadline(const struct app *app);

/* Release every slot during shutdown. */
void i3sd_spawn_shutdown(struct app *app);

#endif
