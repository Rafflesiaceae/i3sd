#ifndef I3SD_PIPEWIRE_H
#define I3SD_PIPEWIRE_H

#include "runtime.h"

struct i3sd_pipewire_source *i3sd_pipewire_create(struct app *app);
void i3sd_pipewire_destroy(struct i3sd_pipewire_source *source);

void i3sd_pipewire_register_lua(lua_State *lua);
int i3sd_pipewire_watch(lua_State *lua, struct i3sd_pipewire_source *source,
                        struct block *block, int callback_index);
void i3sd_pipewire_cancel_owner(struct i3sd_pipewire_source *source,
                                struct block *block);
void i3sd_pipewire_retire_generation(struct i3sd_pipewire_source *source,
                                     struct generation *generation);

void i3sd_pipewire_notify(struct i3sd_pipewire_source *source);
void i3sd_pipewire_reconcile(struct i3sd_pipewire_source *source,
                             uint64_t now_ns);
void i3sd_pipewire_process(struct i3sd_pipewire_source *source,
                           uint64_t now_ns);
uint64_t i3sd_pipewire_deadline(const struct i3sd_pipewire_source *source);

#endif
