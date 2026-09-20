#ifndef I3SD_SPAWN_H
#define I3SD_SPAWN_H

#include "runtime.h"

bool i3sd_spawn_open(struct app *app, struct block *block, char *const argv[],
                     const char *input, size_t input_len, size_t output_limit,
                     int callback_ref, uint64_t serial);
void i3sd_spawn_cancel(struct app *app);
void i3sd_spawn_finish(struct app *app);
void i3sd_spawn_read(struct app *app);

#endif
