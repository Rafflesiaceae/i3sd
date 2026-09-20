#ifndef I3SD_DBUS_H
#define I3SD_DBUS_H

#include <lua.h>

#include <stdbool.h>
#include <stdint.h>

struct i3sd_dbus_generation;
struct i3sd_dbus_runtime;

struct i3sd_dbus_host {
    bool (*owner_active)(void *owner);
    void (*fault_owner)(void *owner);
    void (*log_lua_error)(void *owner, const char *phase);
    void (*push_uint64)(lua_State *lua, uint64_t value);
    void (*push_int64)(lua_State *lua, int64_t value);
};

struct i3sd_dbus_generation *
i3sd_dbus_generation_create(lua_State *lua, const struct i3sd_dbus_host *host);
void i3sd_dbus_generation_deactivate(struct i3sd_dbus_generation *generation);
void i3sd_dbus_generation_destroy(struct i3sd_dbus_generation *generation);
void i3sd_dbus_cancel_owner(struct i3sd_dbus_generation *generation,
                            void *owner);

void i3sd_dbus_register_lua(struct i3sd_dbus_generation *generation,
                            int i3sd_table);
int i3sd_dbus_push_bus(lua_State *lua, struct i3sd_dbus_generation *generation,
                       void *owner, const char *scope);

struct i3sd_dbus_runtime *
i3sd_dbus_runtime_create(int epoll_fd, uint64_t *next_registration_cookie);
void i3sd_dbus_runtime_destroy(struct i3sd_dbus_runtime *runtime);
void i3sd_dbus_reconcile(struct i3sd_dbus_runtime *runtime,
                         struct i3sd_dbus_generation *generation,
                         uint64_t now_ns);
bool i3sd_dbus_process_cookie(struct i3sd_dbus_runtime *runtime,
                              struct i3sd_dbus_generation *generation,
                              uint64_t cookie, uint64_t now_ns);
void i3sd_dbus_process(struct i3sd_dbus_runtime *runtime,
                       struct i3sd_dbus_generation *generation,
                       uint64_t now_ns);
uint64_t i3sd_dbus_deadline(const struct i3sd_dbus_runtime *runtime,
                            const struct i3sd_dbus_generation *generation);

#endif
