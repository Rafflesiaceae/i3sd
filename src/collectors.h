#ifndef I3SD_COLLECTORS_H
#define I3SD_COLLECTORS_H

#include <lua.h>

#include <stdint.h>

struct i3sd_collector_host {
    /* Keep integer cdata construction and the monotonic clock core-owned. */
    uint64_t (*now_ns)(void);
    void (*push_uint64)(lua_State *lua, uint64_t value);
    void (*push_int64)(lua_State *lua, int64_t value);
};

/* Push one collector result (and optional error) onto the Lua stack. */
int i3sd_collect(lua_State *lua, const char *kind, int options,
                 const struct i3sd_collector_host *host);

#endif
