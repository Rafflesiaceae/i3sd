#define _GNU_SOURCE

#include "config.h"
#include "dbus.h"
#include "i3sd/buffer.h"
#include "i3sd/click.h"
#include "i3sd/output.h"
#include "i3sd/timer.h"
#include "i3sd/utf8.h"
#include "power_profiles.h"
#include "runtime.h"
#include "spawn.h"
#include "systemd.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <xxhash.h>
#include <yyjson.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define I3SD_MAX_TEXT 6144U
#define I3SD_MAX_BLOCK_JSON 7168U
#define I3SD_MAX_CONFIG (1024U * 1024U)
#define I3SD_EPOLL_EVENTS 64
#define I3SD_TIMER_BUDGET 256U
#define I3SD_OUTPUT_BUDGET (1024U * 1024U)
#define I3SD_RENDER_INTERVAL_NS 50000000ULL
#define I3SD_SPAWN_ARG_LIMIT 64U
#define I3SD_SPAWN_ARG_BYTES 4096U
#define I3SD_SPAWN_ARGV_BYTES (64U * 1024U)
#define I3SD_SPAWN_INPUT_BYTES (64U * 1024U)
#define I3SD_SPAWN_OUTPUT_BYTES (64U * 1024U)

struct lua_context {
    struct block *block;
};

struct lua_timer_handle {
    struct logical_timer *timer;
};

struct lua_systemd_handle {
    struct systemd_subscription *subscription;
};

struct lua_power_profiles_handle {
    struct power_profiles_subscription *subscription;
};

struct lua_spawn_handle {
    struct app *app;
    uint64_t serial;
};

static const char protocol_prelude[] =
    "{\"version\":1,\"click_events\":true}\n[\n";
static const char context_metatable[] = "i3sd.context";
static const char timer_metatable[] = "i3sd.timer";
static const char systemd_handle_metatable[] = "i3sd.systemd_handle";
static const char power_profiles_handle_metatable[] =
    "i3sd.power_profiles_handle";
static const char spawn_handle_metatable[] = "i3sd.spawn_handle";

static bool make_nonblocking(int fd);
static void push_error(lua_State *lua, const char *code, const char *message,
                       const char *source, int error_number);
static void push_uint64(lua_State *lua, uint64_t value);
static void push_int64(lua_State *lua, int64_t value);

static uint64_t monotonic_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static char *copy_bytes(const char *value, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
}

void i3sd_log_lua_error(struct block *block, const char *phase) {
    const char *message = lua_tostring(block->generation->lua, -1);
    fprintf(stderr, "i3sd: block %s %s failed: %s\n", block->name, phase,
            message == NULL ? "unknown Lua error" : message);
    lua_pop(block->generation->lua, 1);
}

static bool valid_text(const char *text, size_t len, size_t limit) {
    return len <= limit && i3sd_valid_utf8(text, len);
}

static void block_state_destroy(struct block_state *state) {
    free(state->full_text);
    free(state->short_text);
    free(state->color);
    free(state->background);
    free(state->border);
    free(state->min_width_string);
    free(state->align);
    free(state->markup);
    *state = (struct block_state){0};
}

static void block_destroy(struct block *block) {
    if (block == NULL) {
        return;
    }
    block_state_destroy(&block->state);
    i3sd_buffer_destroy(&block->fragment);
    free(block->name);
    free(block->key);
    free(block);
}

static bool identity_equal(const struct identity *identity, const char *name,
                           const char *key, bool has_key) {
    return identity->has_key == has_key && strcmp(identity->name, name) == 0 &&
           (!has_key || strcmp(identity->key, key) == 0);
}

static bool assign_token(struct app *app, struct block *block) {
    for (size_t index = 0; index < app->identity_count; index++) {
        if (identity_equal(&app->identities[index], block->name, block->key,
                           block->has_key)) {
            memcpy(block->token, app->identities[index].token,
                   sizeof(block->token));
            return true;
        }
    }
    if (app->identity_count == 4096) {
        return false;
    }

    unsigned char random[16];
    ssize_t received;
    do {
        received = getrandom(random, sizeof(random), 0);
    } while (received < 0 && errno == EINTR);
    if (received != (ssize_t)sizeof(random)) {
        return false;
    }
    for (size_t index = 0; index < sizeof(random); index++) {
        snprintf(block->token + index * 2, 3, "%02x", random[index]);
    }

    struct identity *identity = &app->identities[app->identity_count];
    identity->name = strdup(block->name);
    identity->key = block->has_key ? strdup(block->key) : NULL;
    if (identity->name == NULL || (block->has_key && identity->key == NULL)) {
        free(identity->name);
        free(identity->key);
        return false;
    }
    identity->has_key = block->has_key;
    memcpy(identity->token, block->token, sizeof(identity->token));
    app->identity_count++;
    return true;
}

static struct lua_context *check_context(lua_State *lua, int index) {
    return luaL_checkudata(lua, index, context_metatable);
}

static bool strict_known_key(const char *key, const char *const *known,
                             size_t known_count) {
    for (size_t index = 0; index < known_count; index++) {
        if (strcmp(key, known[index]) == 0) {
            return true;
        }
    }
    return false;
}

static int absolute_lua_index(lua_State *lua, int index) {
    if (index > 0 || index <= LUA_REGISTRYINDEX) {
        return index;
    }
    return lua_gettop(lua) + index + 1;
}

static void check_strict_table(lua_State *lua, int index,
                               const char *const *known, size_t known_count) {
    index = absolute_lua_index(lua, index);
    lua_pushnil(lua);
    while (lua_next(lua, index) != 0) {
        if (lua_type(lua, -2) != LUA_TSTRING ||
            !strict_known_key(lua_tostring(lua, -2), known, known_count)) {
            luaL_error(lua, "unknown option '%s'",
                       lua_type(lua, -2) == LUA_TSTRING ? lua_tostring(lua, -2)
                                                        : "<non-string>");
        }
        lua_pop(lua, 1);
    }
}

static char *optional_text_field(lua_State *lua, int table, const char *field,
                                 size_t limit) {
    lua_getfield(lua, table, field);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return NULL;
    }
    size_t len;
    const char *value = luaL_checklstring(lua, -1, &len);
    if (!valid_text(value, len, limit)) {
        luaL_error(lua, "%s must be valid, NUL-free UTF-8 of at most %zu bytes",
                   field, limit);
    }
    char *copy = copy_bytes(value, len);
    lua_pop(lua, 1);
    if (copy == NULL) {
        luaL_error(lua, "out of memory copying %s", field);
    }
    return copy;
}

static bool optional_boolean_field(lua_State *lua, int table,
                                   const char *field) {
    lua_getfield(lua, table, field);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return false;
    }
    luaL_checktype(lua, -1, LUA_TBOOLEAN);
    bool result = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    return result;
}

static int optional_border_field(lua_State *lua, int table, const char *field) {
    lua_getfield(lua, table, field);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return 0;
    }
    const lua_Integer value = luaL_checkinteger(lua, -1);
    lua_pop(lua, 1);
    if (value < 0 || value > INT_MAX) {
        luaL_error(lua, "%s must be a non-negative integer", field);
    }
    return (int)value;
}

static bool valid_color(const char *color) {
    if (color == NULL) {
        return true;
    }
    const size_t len = strlen(color);
    if ((len != 7 && len != 9) || color[0] != '#') {
        return false;
    }
    for (size_t index = 1; index < len; index++) {
        const char byte = color[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
              (byte >= 'A' && byte <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool enum_value(const char *value, const char *first, const char *second,
                       const char *third) {
    return value == NULL || strcmp(value, first) == 0 ||
           strcmp(value, second) == 0 ||
           (third != NULL && strcmp(value, third) == 0);
}

static bool serialize_block(struct block *block, struct block_state *state,
                            struct i3sd_buffer *fragment) {
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    if (document == NULL) {
        return false;
    }
    yyjson_mut_val *object = yyjson_mut_obj(document);
    yyjson_mut_doc_set_root(document, object);

#define ADD_STRING(field, value)                                               \
    do {                                                                       \
        if ((value) != NULL &&                                                 \
            !yyjson_mut_obj_add_strcpy(document, object, (field), (value))) {  \
            yyjson_mut_doc_free(document);                                     \
            return false;                                                      \
        }                                                                      \
    } while (0)

    ADD_STRING("full_text", state->full_text);
    ADD_STRING("short_text", state->short_text);
    ADD_STRING("color", state->color);
    ADD_STRING("background", state->background);
    ADD_STRING("border", state->border);
    if (state->border_top != 0) {
        yyjson_mut_obj_add_int(document, object, "border_top",
                               state->border_top);
    }
    if (state->border_right != 0) {
        yyjson_mut_obj_add_int(document, object, "border_right",
                               state->border_right);
    }
    if (state->border_bottom != 0) {
        yyjson_mut_obj_add_int(document, object, "border_bottom",
                               state->border_bottom);
    }
    if (state->border_left != 0) {
        yyjson_mut_obj_add_int(document, object, "border_left",
                               state->border_left);
    }
    if (state->min_width_present) {
        if (state->min_width_is_string) {
            ADD_STRING("min_width", state->min_width_string);
        } else {
            yyjson_mut_obj_add_sint(document, object, "min_width",
                                    state->min_width_integer);
        }
    }
    ADD_STRING("align", state->align);
    if (state->urgent) {
        yyjson_mut_obj_add_bool(document, object, "urgent", true);
    }
    ADD_STRING("markup", state->markup);
    ADD_STRING("name", block->name);
    ADD_STRING("instance", block->token);
    yyjson_mut_obj_add_bool(document, object, "separator", false);
    yyjson_mut_obj_add_int(document, object, "separator_block_width", 9);
#undef ADD_STRING

    size_t json_len = 0;
    char *json = yyjson_mut_write(document, 0, &json_len);
    yyjson_mut_doc_free(document);
    if (json == NULL || json_len > I3SD_MAX_BLOCK_JSON) {
        free(json);
        return false;
    }
    i3sd_buffer_clear(fragment);
    bool success =
        i3sd_buffer_append(fragment, json, json_len, I3SD_MAX_BLOCK_JSON);
    free(json);
    return success;
}

static bool state_equal(const struct block_state *left,
                        const struct block_state *right) {
#define STRING_EQUAL(field)                                                    \
    (((left)->field == NULL && (right)->field == NULL) ||                      \
     ((left)->field != NULL && (right)->field != NULL &&                       \
      strcmp((left)->field, (right)->field) == 0))
    return STRING_EQUAL(full_text) && STRING_EQUAL(short_text) &&
           STRING_EQUAL(color) && STRING_EQUAL(background) &&
           STRING_EQUAL(border) && STRING_EQUAL(min_width_string) &&
           STRING_EQUAL(align) && STRING_EQUAL(markup) &&
           left->min_width_integer == right->min_width_integer &&
           left->border_top == right->border_top &&
           left->border_right == right->border_right &&
           left->border_bottom == right->border_bottom &&
           left->border_left == right->border_left &&
           left->min_width_is_string == right->min_width_is_string &&
           left->min_width_present == right->min_width_present &&
           left->urgent == right->urgent && left->visible == right->visible;
#undef STRING_EQUAL
}

static int lua_context_set(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct block *block = context->block;
    luaL_checktype(lua, 2, LUA_TTABLE);
    static const char *const fields[] = {
        "full_text",   "short_text", "color",        "background",
        "border",      "border_top", "border_right", "border_bottom",
        "border_left", "min_width",  "align",        "urgent",
        "markup",
    };
    check_strict_table(lua, 2, fields, sizeof(fields) / sizeof(fields[0]));

    struct block_state candidate = {0};
    candidate.full_text =
        optional_text_field(lua, 2, "full_text", I3SD_MAX_TEXT);
    if (candidate.full_text == NULL) {
        return luaL_error(lua, "full_text is required");
    }
    candidate.short_text =
        optional_text_field(lua, 2, "short_text", I3SD_MAX_TEXT);
    candidate.color = optional_text_field(lua, 2, "color", 9);
    candidate.background = optional_text_field(lua, 2, "background", 9);
    candidate.border = optional_text_field(lua, 2, "border", 9);
    candidate.align = optional_text_field(lua, 2, "align", 8);
    candidate.markup = optional_text_field(lua, 2, "markup", 8);
    candidate.border_top = optional_border_field(lua, 2, "border_top");
    candidate.border_right = optional_border_field(lua, 2, "border_right");
    candidate.border_bottom = optional_border_field(lua, 2, "border_bottom");
    candidate.border_left = optional_border_field(lua, 2, "border_left");
    candidate.urgent = optional_boolean_field(lua, 2, "urgent");
    candidate.visible = candidate.full_text[0] != '\0';

    lua_getfield(lua, 2, "min_width");
    if (!lua_isnil(lua, -1)) {
        candidate.min_width_present = true;
        if (lua_type(lua, -1) == LUA_TSTRING) {
            size_t len;
            const char *value = lua_tolstring(lua, -1, &len);
            if (!valid_text(value, len, I3SD_MAX_TEXT)) {
                luaL_error(lua, "min_width string is invalid");
            }
            candidate.min_width_string = copy_bytes(value, len);
            candidate.min_width_is_string = true;
        } else {
            const lua_Integer value = luaL_checkinteger(lua, -1);
            if (value < 0) {
                luaL_error(lua, "min_width must be non-negative");
            }
            candidate.min_width_integer = value;
        }
    }
    lua_pop(lua, 1);

    if (!valid_color(candidate.color) || !valid_color(candidate.background) ||
        !valid_color(candidate.border)) {
        block_state_destroy(&candidate);
        return luaL_error(lua, "color fields must use #RRGGBB or #RRGGBBAA");
    }
    if (!enum_value(candidate.align, "left", "center", "right") ||
        !enum_value(candidate.markup, "none", "pango", NULL)) {
        block_state_destroy(&candidate);
        return luaL_error(lua, "invalid align or markup value");
    }

    struct i3sd_buffer fragment = {0};
    if (candidate.visible && !serialize_block(block, &candidate, &fragment)) {
        block_state_destroy(&candidate);
        return luaL_error(lua, "rendered block exceeds the 7 KiB limit");
    }
    const bool changed = !state_equal(&block->state, &candidate);
    if (changed) {
        block_state_destroy(&block->state);
        i3sd_buffer_destroy(&block->fragment);
        block->state = candidate;
        block->fragment = fragment;
        if (!block->generation->staging &&
            block->generation->app->current == block->generation) {
            block->generation->app->render_dirty = true;
        }
    } else {
        block_state_destroy(&candidate);
        i3sd_buffer_destroy(&fragment);
    }
    return 0;
}

static int lua_timer_cancel(lua_State *lua) {
    struct lua_timer_handle *handle = luaL_checkudata(lua, 1, timer_metatable);
    if (handle->timer != NULL && !handle->timer->cancelled) {
        struct app *app = handle->timer->block->generation->app;
        i3sd_timer_cancel(&app->timer_heap, &handle->timer->timer);
        handle->timer->cancelled = true;
    }
    return 0;
}

static bool seconds_to_ns(lua_State *lua, int index, bool allow_zero,
                          uint64_t *result) {
    const lua_Number seconds = luaL_checknumber(lua, index);
    if (!isfinite(seconds) || seconds < 0 || (!allow_zero && seconds == 0) ||
        seconds > (lua_Number)UINT64_MAX / 1000000000.0) {
        return false;
    }
    const long double nanoseconds = (long double)seconds * 1000000000.0L;
    if (nanoseconds > UINT64_MAX) {
        return false;
    }
    *result = (uint64_t)ceill(nanoseconds);
    if (!allow_zero && *result == 0) {
        return false;
    }
    return true;
}

static int create_lua_timer(lua_State *lua, bool repeating) {
    struct lua_context *context = check_context(lua, 1);
    struct generation *generation = context->block->generation;
    uint64_t delay_ns;
    if (!seconds_to_ns(lua, 2, !repeating, &delay_ns)) {
        return luaL_error(lua, "invalid timer duration");
    }
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    if (generation->timer_count == I3SD_MAX_TIMERS) {
        return luaL_error(lua, "timer limit reached");
    }

    struct logical_timer *timer = calloc(1, sizeof(*timer));
    if (timer == NULL) {
        return luaL_error(lua, "out of memory creating timer");
    }
    timer->block = context->block;
    timer->delay_ns = delay_ns;
    timer->timer.interval_ns = repeating ? delay_ns : 0;
    lua_pushvalue(lua, 3);
    timer->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    timer->next = generation->timers;
    generation->timers = timer;
    generation->timer_count++;

    struct lua_timer_handle *handle = lua_newuserdata(lua, sizeof(*handle));
    handle->timer = timer;
    luaL_getmetatable(lua, timer_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

static int lua_context_after(lua_State *lua) {
    return create_lua_timer(lua, false);
}

static int lua_context_every(lua_State *lua) {
    return create_lua_timer(lua, true);
}

static int lua_systemd_cancel(lua_State *lua) {
    struct lua_systemd_handle *handle =
        luaL_checkudata(lua, 1, systemd_handle_metatable);
    if (handle->subscription != NULL) {
        handle->subscription->cancelled = true;
    }
    return 0;
}

static int lua_context_watch_systemd_failed(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct generation *generation = context->block->generation;
    const char *scope_name = luaL_checkstring(lua, 2);
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    if (generation->subscription_count == I3SD_MAX_TIMERS) {
        return luaL_error(lua, "subscription limit reached");
    }

    enum systemd_scope scope;
    if (strcmp(scope_name, "system") == 0) {
        scope = SYSTEMD_SCOPE_SYSTEM;
    } else if (strcmp(scope_name, "user") == 0) {
        scope = SYSTEMD_SCOPE_USER;
    } else if (strcmp(scope_name, "both") == 0) {
        scope = SYSTEMD_SCOPE_BOTH;
    } else {
        return luaL_error(lua, "systemd scope must be system, user, or both");
    }

    struct systemd_subscription *subscription =
        calloc(1, sizeof(*subscription));
    if (subscription == NULL) {
        return luaL_error(lua, "out of memory creating systemd subscription");
    }
    subscription->block = context->block;
    subscription->scope = scope;
    lua_pushvalue(lua, 3);
    subscription->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    subscription->next = generation->systemd_subscriptions;
    generation->systemd_subscriptions = subscription;
    generation->subscription_count++;

    struct lua_systemd_handle *handle = lua_newuserdata(lua, sizeof(*handle));
    handle->subscription = subscription;
    luaL_getmetatable(lua, systemd_handle_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

static int lua_power_profiles_cancel(lua_State *lua) {
    struct lua_power_profiles_handle *handle =
        luaL_checkudata(lua, 1, power_profiles_handle_metatable);
    if (handle->subscription != NULL) {
        handle->subscription->cancelled = true;
    }
    return 0;
}

static int lua_context_watch_power_profiles(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct generation *generation = context->block->generation;
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    if (generation->subscription_count == I3SD_MAX_TIMERS) {
        return luaL_error(lua, "subscription limit reached");
    }
    struct power_profiles_subscription *subscription =
        calloc(1, sizeof(*subscription));
    if (subscription == NULL) {
        return luaL_error(lua,
                          "out of memory creating power profile subscription");
    }
    subscription->block = context->block;
    lua_pushvalue(lua, 2);
    subscription->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    subscription->next = generation->power_profiles_subscriptions;
    generation->power_profiles_subscriptions = subscription;
    generation->subscription_count++;

    struct lua_power_profiles_handle *handle =
        lua_newuserdata(lua, sizeof(*handle));
    handle->subscription = subscription;
    luaL_getmetatable(lua, power_profiles_handle_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

static int lua_spawn_cancel(lua_State *lua) {
    struct lua_spawn_handle *handle =
        luaL_checkudata(lua, 1, spawn_handle_metatable);
    if (handle->app->spawn.active &&
        handle->app->spawn.serial == handle->serial) {
        i3sd_spawn_cancel(handle->app);
    }
    return 0;
}

static int lua_context_spawn(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct block *block = context->block;
    luaL_checktype(lua, 2, LUA_TTABLE);
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    static const char *const fields[] = {"argv", "stdin", "stdout_limit"};
    check_strict_table(lua, 2, fields, sizeof(fields) / sizeof(fields[0]));

    lua_getfield(lua, 2, "argv");
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int argv_index = lua_gettop(lua);
    const size_t argc = lua_objlen(lua, argv_index);
    if (argc == 0 || argc > I3SD_SPAWN_ARG_LIMIT) {
        return luaL_error(lua, "spawn argv must contain 1 to %u entries",
                          I3SD_SPAWN_ARG_LIMIT);
    }

    /* Reject sparse or keyed argv tables so execution is deterministic. */
    size_t encountered = 0;
    lua_pushnil(lua);
    while (lua_next(lua, argv_index) != 0) {
        if (lua_type(lua, -2) != LUA_TNUMBER) {
            return luaL_error(lua,
                              "spawn argv must be a dense 1-based sequence");
        }
        const lua_Number numeric_key = lua_tonumber(lua, -2);
        if (!isfinite(numeric_key) || numeric_key < 1 ||
            numeric_key > (lua_Number)argc ||
            floor(numeric_key) != numeric_key) {
            return luaL_error(lua,
                              "spawn argv must be a dense 1-based sequence");
        }
        encountered++;
        lua_pop(lua, 1);
    }
    if (encountered != argc) {
        return luaL_error(lua, "spawn argv must be a dense 1-based sequence");
    }

    size_t argv_bytes = 0;
    for (size_t index = 1; index <= argc; index++) {
        lua_rawgeti(lua, argv_index, (int)index);
        if (lua_type(lua, -1) != LUA_TSTRING) {
            return luaL_error(lua, "spawn argv entry %zu must be a string",
                              index);
        }
        size_t argument_len;
        const char *argument = lua_tolstring(lua, -1, &argument_len);
        if (index == 1 && argument_len == 0) {
            return luaL_error(lua, "spawn executable must be non-empty");
        }
        if (argument_len > I3SD_SPAWN_ARG_BYTES ||
            memchr(argument, '\0', argument_len) != NULL) {
            return luaL_error(lua,
                              "spawn argv entry %zu must be NUL-free and at "
                              "most %u bytes",
                              index, I3SD_SPAWN_ARG_BYTES);
        }
        if (argument_len + 1 > I3SD_SPAWN_ARGV_BYTES - argv_bytes) {
            return luaL_error(lua, "spawn argv exceeds the %u-byte limit",
                              I3SD_SPAWN_ARGV_BYTES);
        }
        argv_bytes += argument_len + 1;
        lua_pop(lua, 1);
    }

    lua_getfield(lua, 2, "stdin");
    size_t input_len = 0;
    const char *input = "";
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TSTRING);
        input = lua_tolstring(lua, -1, &input_len);
        if (input_len > I3SD_SPAWN_INPUT_BYTES) {
            return luaL_error(lua, "spawn stdin exceeds the %u-byte limit",
                              I3SD_SPAWN_INPUT_BYTES);
        }
    }

    lua_getfield(lua, 2, "stdout_limit");
    size_t output_limit = I3SD_SPAWN_OUTPUT_BYTES;
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TNUMBER);
        const lua_Number requested = lua_tonumber(lua, -1);
        if (!isfinite(requested) || requested < 1 ||
            requested > I3SD_SPAWN_OUTPUT_BYTES ||
            floor(requested) != requested) {
            return luaL_error(lua, "spawn stdout_limit must be from 1 to %u",
                              I3SD_SPAWN_OUTPUT_BYTES);
        }
        output_limit = (size_t)requested;
    }

    struct app *app = block->generation->app;
    if (app->current != block->generation || block->faulted ||
        app->spawn.active) {
        lua_pushnil(lua);
        return 1;
    }

    char **argv = calloc(argc + 1, sizeof(*argv));
    if (argv == NULL) {
        return luaL_error(lua, "out of memory creating spawn argv");
    }
    for (size_t index = 1; index <= argc; index++) {
        lua_rawgeti(lua, argv_index, (int)index);
        argv[index - 1] = (char *)lua_tostring(lua, -1);
        lua_pop(lua, 1);
    }

    lua_pushvalue(lua, 3);
    const int callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    uint64_t serial = ++app->next_spawn_serial;
    if (serial == 0) {
        serial = ++app->next_spawn_serial;
    }
    const bool opened = i3sd_spawn_open(app, block, argv, input, input_len,
                                        output_limit, callback_ref, serial);
    free(argv);
    if (!opened) {
        luaL_unref(lua, LUA_REGISTRYINDEX, callback_ref);
        lua_pushnil(lua);
        return 1;
    }

    struct lua_spawn_handle *handle = lua_newuserdata(lua, sizeof(*handle));
    handle->app = app;
    handle->serial = serial;
    luaL_getmetatable(lua, spawn_handle_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

static int lua_context_set_power_profile(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct block *block = context->block;
    size_t profile_len;
    const char *profile = luaL_checklstring(lua, 2, &profile_len);
    if (!valid_text(profile, profile_len, I3SD_POWER_PROFILE_NAME_LIMIT)) {
        return luaL_error(lua,
                          "power profile must be valid, NUL-free UTF-8 "
                          "of at most %u bytes",
                          I3SD_POWER_PROFILE_NAME_LIMIT);
    }
    struct app *app = block->generation->app;
    const bool current = app->current == block->generation && !block->faulted;
    const bool accepted =
        current && app->power_profiles.profiles_valid &&
        i3sd_power_profile_known(&app->power_profiles, profile) &&
        i3sd_power_profile_set(app, profile);
    lua_pushboolean(lua, accepted);
    return 1;
}

static void push_error(lua_State *lua, const char *code, const char *message,
                       const char *source, int error_number) {
    lua_newtable(lua);
    lua_pushstring(lua, code);
    lua_setfield(lua, -2, "code");
    lua_pushstring(lua, message);
    lua_setfield(lua, -2, "message");
    if (source != NULL) {
        lua_pushstring(lua, source);
        lua_setfield(lua, -2, "source");
    }
    if (error_number != 0) {
        lua_pushinteger(lua, error_number);
        lua_setfield(lua, -2, "errno");
    }
}

static void push_uint64(lua_State *lua, uint64_t value) {
    struct generation *generation;
    lua_getfield(lua, LUA_REGISTRYINDEX, "i3sd.current_generation");
    generation = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, generation->push_uint64_ref);
    lua_pushnumber(lua, (lua_Number)(value >> 32U));
    lua_pushnumber(lua, (lua_Number)(value & UINT32_MAX));
    if (lua_pcall(lua, 2, 1, 0) != 0) {
        lua_error(lua);
    }
}

static void push_int64(lua_State *lua, int64_t value) {
    struct generation *generation;
    lua_getfield(lua, LUA_REGISTRYINDEX, "i3sd.current_generation");
    generation = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, generation->push_int64_ref);
    lua_pushnumber(lua, (lua_Number)(value >> 32U));
    lua_pushnumber(lua, (lua_Number)((uint64_t)value & UINT32_MAX));
    if (lua_pcall(lua, 2, 1, 0) != 0) {
        lua_error(lua);
    }
}

static void push_snapshot_header(lua_State *lua, const char *source_id) {
    lua_newtable(lua);
    push_uint64(lua, monotonic_now_ns());
    lua_setfield(lua, -2, "timestamp_ns");
    lua_pushstring(lua, source_id);
    lua_setfield(lua, -2, "source_id");
    push_uint64(lua, 1);
    lua_setfield(lua, -2, "continuity");
}

static bool option_boolean(lua_State *lua, int table, const char *name,
                           bool fallback) {
    lua_getfield(lua, table, name);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return fallback;
    }
    luaL_checktype(lua, -1, LUA_TBOOLEAN);
    bool value = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    return value;
}

static int sample_cpu(lua_State *lua, int options) {
    static const char *const fields[] = {"per_cpu"};
    check_strict_table(lua, options, fields, 1);
    const bool per_cpu = option_boolean(lua, options, "per_cpu", false);
    FILE *file = fopen("/proc/stat", "re");
    if (file == NULL) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "cpu", errno);
        return 2;
    }
    push_snapshot_header(lua, "proc-stat");
    lua_newtable(lua);
    int cpu_index = 1;
    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        char id[32];
        unsigned long long values[10] = {0};
        const int parsed = sscanf(
            line, "%31s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", id,
            &values[0], &values[1], &values[2], &values[3], &values[4],
            &values[5], &values[6], &values[7], &values[8], &values[9]);
        if (parsed != 11 || strncmp(id, "cpu", 3) != 0) {
            if (strncmp(line, "cpu", 3) != 0) {
                break;
            }
            continue;
        }
        lua_newtable(lua);
        lua_pushstring(lua, id);
        lua_setfield(lua, -2, "id");
        static const char *const names[10] = {
            "user", "nice",    "system", "idle",  "iowait",
            "irq",  "softirq", "steal",  "guest", "guest_nice",
        };
        for (size_t index = 0; index < 10; index++) {
            push_uint64(lua, values[index]);
            lua_setfield(lua, -2, names[index]);
        }
        if (strcmp(id, "cpu") == 0) {
            lua_setfield(lua, -3, "aggregate");
        } else if (per_cpu) {
            lua_rawseti(lua, -2, cpu_index++);
        } else {
            lua_pop(lua, 1);
        }
    }
    fclose(file);
    if (per_cpu) {
        lua_setfield(lua, -2, "cpus");
    } else {
        lua_pop(lua, 1);
    }
    return 1;
}

static int sample_load(lua_State *lua, int options) {
    check_strict_table(lua, options, NULL, 0);
    struct sysinfo information;
    if (sysinfo(&information) < 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "load", errno);
        return 2;
    }
    push_snapshot_header(lua, "sysinfo-load");
    const double scale = 1.0 / (double)(1U << SI_LOAD_SHIFT);
    lua_pushnumber(lua, information.loads[0] * scale);
    lua_setfield(lua, -2, "load1");
    lua_pushnumber(lua, information.loads[1] * scale);
    lua_setfield(lua, -2, "load5");
    lua_pushnumber(lua, information.loads[2] * scale);
    lua_setfield(lua, -2, "load15");
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    lua_pushinteger(lua, cpus > 0 ? cpus : 1);
    lua_setfield(lua, -2, "online_cpus");
    return 1;
}

static int sample_memory(lua_State *lua, int options) {
    check_strict_table(lua, options, NULL, 0);
    FILE *file = fopen("/proc/meminfo", "re");
    if (file == NULL) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "memory", errno);
        return 2;
    }
    uint64_t total = 0, available = 0, swap_total = 0, swap_free = 0;
    char key[64], unit[16];
    unsigned long long value;
    while (fscanf(file, "%63[^:]: %llu %15s\n", key, &value, unit) == 3) {
        const uint64_t bytes = value * 1024ULL;
        if (strcmp(key, "MemTotal") == 0) {
            total = bytes;
        } else if (strcmp(key, "MemAvailable") == 0) {
            available = bytes;
        } else if (strcmp(key, "SwapTotal") == 0) {
            swap_total = bytes;
        } else if (strcmp(key, "SwapFree") == 0) {
            swap_free = bytes;
        }
    }
    fclose(file);
    if (total == 0 || available == 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", "incomplete /proc/meminfo", "memory", 0);
        return 2;
    }
    push_snapshot_header(lua, "proc-meminfo");
    push_uint64(lua, total);
    lua_setfield(lua, -2, "mem_total");
    push_uint64(lua, available);
    lua_setfield(lua, -2, "mem_available");
    push_uint64(lua, swap_total);
    lua_setfield(lua, -2, "swap_total");
    push_uint64(lua, swap_free);
    lua_setfield(lua, -2, "swap_free");
    return 1;
}

static int sample_filesystem(lua_State *lua, int options) {
    static const char *const fields[] = {"path"};
    check_strict_table(lua, options, fields, 1);
    lua_getfield(lua, options, "path");
    size_t path_len;
    const char *path = luaL_checklstring(lua, -1, &path_len);
    if (!valid_text(path, path_len, PATH_MAX)) {
        return luaL_error(lua, "path must be valid, NUL-free UTF-8");
    }
    struct statvfs status;
    if (statvfs(path, &status) < 0) {
        const int saved_errno = errno;
        lua_pop(lua, 1);
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(saved_errno), "filesystem",
                   saved_errno);
        return 2;
    }
    push_snapshot_header(lua, path);
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "path");
    push_uint64(lua, (uint64_t)status.f_blocks * status.f_frsize);
    lua_setfield(lua, -2, "total_bytes");
    push_uint64(lua, (uint64_t)status.f_bfree * status.f_frsize);
    lua_setfield(lua, -2, "free_bytes");
    push_uint64(lua, (uint64_t)status.f_bavail * status.f_frsize);
    lua_setfield(lua, -2, "available_bytes");
    lua_pushboolean(lua, (status.f_flag & ST_RDONLY) != 0);
    lua_setfield(lua, -2, "readonly");
    lua_remove(lua, -2);
    return 1;
}

static int sample_time(lua_State *lua, int options) {
    check_strict_table(lua, options, NULL, 0);
    struct timespec realtime, monotonic, boottime;
    if (clock_gettime(CLOCK_REALTIME, &realtime) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0 ||
        clock_gettime(CLOCK_BOOTTIME, &boottime) < 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "time", errno);
        return 2;
    }
    push_snapshot_header(lua, "linux-clocks");
#define SET_CLOCK(name, value)                                                 \
    do {                                                                       \
        push_uint64(lua, (uint64_t)(value).tv_sec * 1000000000ULL +            \
                             (uint64_t)(value).tv_nsec);                       \
        lua_setfield(lua, -2, (name));                                         \
    } while (0)
    SET_CLOCK("realtime_ns", realtime);
    SET_CLOCK("monotonic_ns", monotonic);
    SET_CLOCK("boottime_ns", boottime);
#undef SET_CLOCK
    return 1;
}

static int sample_psi(lua_State *lua, int options) {
    static const char *const fields[] = {"resource"};
    check_strict_table(lua, options, fields, 1);
    lua_getfield(lua, options, "resource");
    const char *resource = luaL_checkstring(lua, -1);
    if (strcmp(resource, "cpu") != 0 && strcmp(resource, "memory") != 0 &&
        strcmp(resource, "io") != 0) {
        return luaL_error(lua, "PSI resource must be cpu, memory, or io");
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        const int saved_errno = errno;
        lua_pop(lua, 1);
        lua_pushnil(lua);
        push_error(lua, saved_errno == ENOENT ? "unsupported" : "unavailable",
                   strerror(saved_errno), "psi", saved_errno);
        return 2;
    }
    push_snapshot_header(lua, path);
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char mode[16];
        double avg10, avg60, avg300;
        unsigned long long total;
        if (sscanf(line, "%15s avg10=%lf avg60=%lf avg300=%lf total=%llu", mode,
                   &avg10, &avg60, &avg300, &total) != 5) {
            continue;
        }
        lua_newtable(lua);
        lua_pushnumber(lua, avg10);
        lua_setfield(lua, -2, "avg10");
        lua_pushnumber(lua, avg60);
        lua_setfield(lua, -2, "avg60");
        lua_pushnumber(lua, avg300);
        lua_setfield(lua, -2, "avg300");
        push_uint64(lua, total);
        lua_setfield(lua, -2, "total_us");
        lua_setfield(lua, -2, mode);
    }
    fclose(file);
    lua_remove(lua, -2);
    return 1;
}

static int sample_file_stat(lua_State *lua, int options) {
    static const char *const fields[] = {"path", "follow"};
    check_strict_table(lua, options, fields, 2);
    lua_getfield(lua, options, "path");
    size_t path_len;
    const char *path = luaL_checklstring(lua, -1, &path_len);
    if (!valid_text(path, path_len, PATH_MAX)) {
        return luaL_error(lua, "path must be valid, NUL-free UTF-8");
    }
    const bool follow = option_boolean(lua, options, "follow", true);
    struct stat status;
    const int result = follow ? stat(path, &status) : lstat(path, &status);
    if (result < 0 && errno != ENOENT && errno != ENOTDIR) {
        const int saved_errno = errno;
        lua_pop(lua, 1);
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(saved_errno), "file_stat",
                   saved_errno);
        return 2;
    }
    push_snapshot_header(lua, path);
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "path");
    lua_pushboolean(lua, result == 0);
    lua_setfield(lua, -2, "exists");
    if (result == 0) {
        if (status.st_size < 0) {
            lua_pop(lua, 2);
            lua_pushnil(lua);
            push_error(lua, "too_large", "negative file size is unsupported",
                       "file_stat", 0);
            return 2;
        }
        push_uint64(lua, (uint64_t)status.st_size);
        lua_setfield(lua, -2, "size_bytes");
        push_uint64(lua, status.st_dev);
        lua_setfield(lua, -2, "device");
        push_uint64(lua, status.st_ino);
        lua_setfield(lua, -2, "inode");
        if (status.st_mtim.tv_sec > INT64_MAX / 1000000000LL ||
            (status.st_mtim.tv_sec == INT64_MAX / 1000000000LL &&
             status.st_mtim.tv_nsec > INT64_MAX % 1000000000LL) ||
            status.st_mtim.tv_sec < INT64_MIN / 1000000000LL) {
            lua_pop(lua, 2);
            lua_pushnil(lua);
            push_error(lua, "too_large", "mtime is outside signed nanoseconds",
                       "file_stat", 0);
            return 2;
        }
        const int64_t mtime_ns =
            status.st_mtim.tv_sec * 1000000000LL + status.st_mtim.tv_nsec;
        push_int64(lua, mtime_ns);
        lua_setfield(lua, -2, "mtime_ns");
        const char *kind = S_ISREG(status.st_mode)   ? "regular"
                           : S_ISDIR(status.st_mode) ? "directory"
                           : S_ISLNK(status.st_mode) ? "symlink"
                                                     : "other";
        lua_pushstring(lua, kind);
        lua_setfield(lua, -2, "kind");
    }
    lua_remove(lua, -2);
    return 1;
}

static int lua_context_sample(lua_State *lua) {
    check_context(lua, 1);
    const char *kind = luaL_checkstring(lua, 2);
    if (lua_isnoneornil(lua, 3)) {
        lua_newtable(lua);
    } else {
        luaL_checktype(lua, 3, LUA_TTABLE);
        lua_pushvalue(lua, 3);
    }
    const int options = lua_gettop(lua);
    int result;
    if (strcmp(kind, "cpu") == 0) {
        result = sample_cpu(lua, options);
    } else if (strcmp(kind, "load") == 0) {
        result = sample_load(lua, options);
    } else if (strcmp(kind, "memory") == 0) {
        result = sample_memory(lua, options);
    } else if (strcmp(kind, "filesystem") == 0) {
        result = sample_filesystem(lua, options);
    } else if (strcmp(kind, "time") == 0) {
        result = sample_time(lua, options);
    } else if (strcmp(kind, "psi") == 0) {
        result = sample_psi(lua, options);
    } else if (strcmp(kind, "file_stat") == 0) {
        result = sample_file_stat(lua, options);
    } else {
        return luaL_error(lua, "unknown collector '%s'", kind);
    }
    lua_remove(lua, options);
    return result;
}

static int lua_has_feature(lua_State *lua) {
    const char *feature = luaL_checkstring(lua, 1);
    bool available =
        strcmp(feature, "collectors") == 0 || strcmp(feature, "inotify") == 0 ||
        strcmp(feature, "psi") == 0 || strcmp(feature, "systemd") == 0 ||
        strcmp(feature, "power_profiles") == 0 ||
        strcmp(feature, "spawn") == 0 || strcmp(feature, "dbus") == 0;
    lua_pushboolean(lua, available);
    return 1;
}

static int lua_features(lua_State *lua) {
    lua_newtable(lua);
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "collectors");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "inotify");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "psi");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "systemd");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "power_profiles");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "spawn");
    lua_pushboolean(lua, true);
    lua_setfield(lua, -2, "dbus");
    return 1;
}

static int reference_function(lua_State *lua, int table, const char *field) {
    lua_getfield(lua, table, field);
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return LUA_NOREF;
    }
    luaL_checktype(lua, -1, LUA_TFUNCTION);
    return luaL_ref(lua, LUA_REGISTRYINDEX);
}

static int lua_define_block(lua_State *lua) {
    struct generation *generation = lua_touserdata(lua, lua_upvalueindex(1));
    luaL_checktype(lua, 1, LUA_TTABLE);
    static const char *const fields[] = {"name", "key",    "order", "interval",
                                         "init", "update", "click"};
    check_strict_table(lua, 1, fields, sizeof(fields) / sizeof(fields[0]));
    if (generation->block_count == I3SD_MAX_BLOCKS) {
        return luaL_error(lua, "block limit reached");
    }

    lua_getfield(lua, 1, "name");
    size_t name_len;
    const char *name = luaL_checklstring(lua, -1, &name_len);
    if (!valid_text(name, name_len, 256)) {
        return luaL_error(lua, "block name is invalid");
    }
    struct block *block = calloc(1, sizeof(*block));
    if (block == NULL) {
        return luaL_error(lua, "out of memory creating block");
    }
    block->generation = generation;
    block->name = copy_bytes(name, name_len);
    block->declaration_order = generation->block_count;
    block->init_ref = block->update_ref = block->click_ref = LUA_NOREF;
    lua_pop(lua, 1);

    lua_getfield(lua, 1, "key");
    if (!lua_isnil(lua, -1)) {
        size_t key_len;
        const char *key = luaL_checklstring(lua, -1, &key_len);
        if (!valid_text(key, key_len, 256)) {
            block_destroy(block);
            return luaL_error(lua, "block key is invalid");
        }
        block->key = copy_bytes(key, key_len);
        block->has_key = true;
    }
    lua_pop(lua, 1);

    for (size_t index = 0; index < generation->block_count; index++) {
        struct block *other = generation->blocks[index];
        if (other->has_key == block->has_key &&
            strcmp(other->name, block->name) == 0 &&
            (!block->has_key || strcmp(other->key, block->key) == 0)) {
            block_destroy(block);
            return luaL_error(lua, "duplicate block identity");
        }
    }
    lua_getfield(lua, 1, "order");
    if (!lua_isnil(lua, -1)) {
        block->order = (int)luaL_checkinteger(lua, -1);
    }
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "interval");
    if (!lua_isnil(lua, -1)) {
        block->interval = luaL_checknumber(lua, -1);
        if (!isfinite(block->interval) || block->interval <= 0) {
            block_destroy(block);
            return luaL_error(lua, "interval must be finite and positive");
        }
    }
    lua_pop(lua, 1);
    block->init_ref = reference_function(lua, 1, "init");
    block->update_ref = reference_function(lua, 1, "update");
    block->click_ref = reference_function(lua, 1, "click");
    if (block->interval > 0 && block->update_ref == LUA_NOREF) {
        block_destroy(block);
        return luaL_error(lua, "interval requires update");
    }
    if (!assign_token(generation->app, block)) {
        block_destroy(block);
        return luaL_error(lua, "unable to allocate click identity");
    }

    struct lua_context *context = lua_newuserdata(lua, sizeof(*context));
    context->block = block;
    luaL_getmetatable(lua, context_metatable);
    lua_setmetatable(lua, -2);
    block->context_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    generation->blocks[generation->block_count++] = block;
    return 0;
}

static bool call_block_ref(struct block *block, int reference,
                           const char *phase, int extra_arguments) {
    if (reference == LUA_NOREF || block->faulted) {
        return true;
    }
    lua_State *lua = block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, reference);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
    if (extra_arguments > 0) {
        lua_insert(lua, -(extra_arguments + 1));
    }
    if (lua_pcall(lua, 1 + extra_arguments, 0, 0) != 0) {
        i3sd_log_lua_error(block, phase);
        return false;
    }
    return true;
}

static int compare_blocks(const void *left_pointer, const void *right_pointer) {
    const struct block *left = *(const struct block *const *)left_pointer;
    const struct block *right = *(const struct block *const *)right_pointer;
    if (left->order != right->order) {
        return left->order > right->order ? -1 : 1;
    }
    return left->declaration_order < right->declaration_order   ? -1
           : left->declaration_order > right->declaration_order ? 1
                                                                : 0;
}

static int lua_context_dbus(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    return i3sd_dbus_push_bus(lua, context->block->generation->dbus,
                              context->block, luaL_checkstring(lua, 2));
}

static bool dbus_owner_active(void *owner) {
    struct block *block = owner;
    return !block->faulted &&
           block->generation->app->current == block->generation;
}

static void dbus_fault_owner(void *owner) { i3sd_fault_block(owner); }

static void dbus_log_lua_error(void *owner, const char *phase) {
    i3sd_log_lua_error(owner, phase);
}

static const struct i3sd_dbus_host dbus_host = {
    .owner_active = dbus_owner_active,
    .fault_owner = dbus_fault_owner,
    .log_lua_error = dbus_log_lua_error,
    .push_uint64 = push_uint64,
    .push_int64 = push_int64,
};

static void register_lua_api(struct generation *generation) {
    lua_State *lua = generation->lua;
    luaL_newmetatable(lua, context_metatable);
    lua_newtable(lua);
    static const luaL_Reg context_methods[] = {
        {"set", lua_context_set},
        {"after", lua_context_after},
        {"every", lua_context_every},
        {"sample", lua_context_sample},
        {"spawn", lua_context_spawn},
        {"dbus", lua_context_dbus},
        {"_watch_systemd_failed", lua_context_watch_systemd_failed},
        {"_watch_power_profiles", lua_context_watch_power_profiles},
        {"_set_power_profile", lua_context_set_power_profile},
        {NULL, NULL},
    };
    luaL_register(lua, NULL, context_methods);
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, timer_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_timer_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_timer_cancel);
    lua_setfield(lua, -2, "__gc");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, systemd_handle_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_systemd_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, power_profiles_handle_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_power_profiles_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, spawn_handle_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_spawn_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    lua_newtable(lua);
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "api_version");
    lua_pushstring(lua, I3SD_VERSION);
    lua_setfield(lua, -2, "version");
    lua_pushcfunction(lua, lua_has_feature);
    lua_setfield(lua, -2, "has_feature");
    lua_pushcfunction(lua, lua_features);
    lua_setfield(lua, -2, "features");
    i3sd_dbus_register_lua(generation->dbus, -1);
    lua_setglobal(lua, "i3sd");

    lua_pushlightuserdata(lua, generation);
    lua_pushcclosure(lua, lua_define_block, 1);
    lua_setglobal(lua, "block");

    /* LuaJIT's public C API cannot directly allocate cdata values. */
    const char *integer_helpers = "local ffi = require('ffi')\n"
                                  "return function(hi, lo)\n"
                                  "  return ffi.new('uint64_t', hi) * "
                                  "ffi.new('uint64_t', 4294967296) + lo\n"
                                  "end, function(hi, lo)\n"
                                  "  return ffi.new('int64_t', hi) * "
                                  "ffi.new('int64_t', 4294967296) + lo\n"
                                  "end";
    if (luaL_loadstring(lua, integer_helpers) != 0 ||
        lua_pcall(lua, 0, 2, 0) != 0) {
        fprintf(stderr,
                "i3sd: unable to initialize exact integer helpers: %s\n",
                lua_tostring(lua, -1));
        exit(EXIT_FAILURE);
    }
    generation->push_int64_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    generation->push_uint64_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(lua, generation);
    lua_setfield(lua, LUA_REGISTRYINDEX, "i3sd.current_generation");
}

static bool read_config(const char *path, struct i3sd_buffer *source,
                        XXH128_hash_t *digest, struct stat *metadata) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "i3sd: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat before, after;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || before.st_size > I3SD_MAX_CONFIG) {
        fprintf(stderr,
                "i3sd: config must be a regular file of at most 1 MiB\n");
        close(fd);
        return false;
    }
    i3sd_buffer_clear(source);
    char bytes[16384];
    for (;;) {
        ssize_t count = read(fd, bytes, sizeof(bytes));
        if (count > 0) {
            if (!i3sd_buffer_append(source, bytes, (size_t)count,
                                    I3SD_MAX_CONFIG)) {
                close(fd);
                return false;
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            fprintf(stderr, "i3sd: cannot read %s: %s\n", path,
                    strerror(errno));
            close(fd);
            return false;
        }
    }
    if (fstat(fd, &after) < 0) {
        close(fd);
        return false;
    }
    close(fd);
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        fprintf(stderr, "i3sd: config changed while it was being read\n");
        return false;
    }
    *digest = XXH3_128bits(source->data, source->len);
    *metadata = after;
    return true;
}

static bool running_from_build_tree(void) {
    char executable[PATH_MAX + 1];
    const ssize_t length =
        readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable)) {
        return false;
    }
    executable[length] = '\0';
    return strcmp(executable, I3SD_BUILD_EXECUTABLE) == 0;
}

static void configure_lua_path(struct generation *generation) {
    lua_State *lua = generation->lua;
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, "path");
    const char *old_path = lua_tostring(lua, -1);
    if (running_from_build_tree()) {
        /* Uninstalled builds resolve bundled modules from their source tree. */
        lua_pushfstring(
            lua,
            "%s/?.lua;%s/?/init.lua;%s/?.lua;%s/?/init.lua;%s/?.lua;%s/?/"
            "init.lua;%s",
            generation->app->config_dir, generation->app->config_dir,
            I3SD_SOURCE_LUA_DIR, I3SD_SOURCE_LUA_DIR, I3SD_LUA_DIR,
            I3SD_LUA_DIR, old_path);
    } else {
        lua_pushfstring(lua, "%s/?.lua;%s/?/init.lua;%s/?.lua;%s/?/init.lua;%s",
                        generation->app->config_dir,
                        generation->app->config_dir, I3SD_LUA_DIR, I3SD_LUA_DIR,
                        old_path);
    }
    lua_setfield(lua, -3, "path");
    lua_pop(lua, 2);
}

static void generation_destroy(struct generation *generation) {
    if (generation == NULL) {
        return;
    }
    /* Spawn callbacks and child lifetime are owned by their Lua generation. */
    if (generation->app->spawn.active &&
        generation->app->spawn.block->generation == generation) {
        i3sd_spawn_cancel(generation->app);
    }
    struct logical_timer *timer = generation->timers;
    while (timer != NULL) {
        if (timer->timer.active) {
            i3sd_timer_cancel(&generation->app->timer_heap, &timer->timer);
        }
        timer->cancelled = true;
        timer = timer->next;
    }
    for (struct systemd_subscription *subscription =
             generation->systemd_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        subscription->cancelled = true;
    }
    for (struct power_profiles_subscription *subscription =
             generation->power_profiles_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        subscription->cancelled = true;
    }
    i3sd_dbus_generation_deactivate(generation->dbus);
    /* Lua finalizers can still inspect native handles, so close Lua first. */
    if (generation->lua != NULL) {
        lua_close(generation->lua);
    }
    i3sd_dbus_generation_destroy(generation->dbus);
    timer = generation->timers;
    while (timer != NULL) {
        struct logical_timer *next = timer->next;
        free(timer);
        timer = next;
    }
    struct systemd_subscription *subscription =
        generation->systemd_subscriptions;
    while (subscription != NULL) {
        struct systemd_subscription *next = subscription->next;
        free(subscription);
        subscription = next;
    }
    struct power_profiles_subscription *power_subscription =
        generation->power_profiles_subscriptions;
    while (power_subscription != NULL) {
        struct power_profiles_subscription *next = power_subscription->next;
        free(power_subscription);
        power_subscription = next;
    }
    for (size_t index = 0; index < generation->block_count; index++) {
        block_destroy(generation->blocks[index]);
    }
    free(generation);
}

static struct generation *stage_generation(struct app *app) {
    struct i3sd_buffer source = {0};
    XXH128_hash_t digest, validation_digest;
    struct stat metadata, validation_metadata;
    if (!read_config(app->config_path, &source, &digest, &metadata)) {
        i3sd_buffer_destroy(&source);
        return NULL;
    }
    struct generation *generation = calloc(1, sizeof(*generation));
    if (generation == NULL) {
        i3sd_buffer_destroy(&source);
        return NULL;
    }
    generation->app = app;
    generation->staging = true;
    generation->lua = luaL_newstate();
    if (generation->lua == NULL) {
        generation_destroy(generation);
        i3sd_buffer_destroy(&source);
        return NULL;
    }
    luaL_openlibs(generation->lua);
    generation->dbus = i3sd_dbus_generation_create(generation->lua, &dbus_host);
    if (generation->dbus == NULL) {
        generation_destroy(generation);
        i3sd_buffer_destroy(&source);
        return NULL;
    }
    register_lua_api(generation);
    configure_lua_path(generation);

    if (luaL_loadbuffer(generation->lua, source.data, source.len,
                        app->config_path) != 0 ||
        lua_pcall(generation->lua, 0, 0, 0) != 0) {
        fprintf(stderr, "i3sd: configuration failed: %s\n",
                lua_tostring(generation->lua, -1));
        generation_destroy(generation);
        i3sd_buffer_destroy(&source);
        return NULL;
    }
    for (size_t index = 0; index < generation->block_count; index++) {
        struct block *block = generation->blocks[index];
        if (!call_block_ref(block, block->init_ref, "init", 0) ||
            !call_block_ref(block, block->update_ref, "staging update", 0)) {
            generation_destroy(generation);
            i3sd_buffer_destroy(&source);
            return NULL;
        }
        generation->ordered[index] = block;
    }
    qsort(generation->ordered, generation->block_count,
          sizeof(generation->ordered[0]), compare_blocks);

    struct i3sd_buffer validation = {0};
    bool stable = read_config(app->config_path, &validation, &validation_digest,
                              &validation_metadata) &&
                  XXH128_isEqual(digest, validation_digest) &&
                  metadata.st_dev == validation_metadata.st_dev &&
                  metadata.st_ino == validation_metadata.st_ino;
    i3sd_buffer_destroy(&validation);
    i3sd_buffer_destroy(&source);
    if (!stable) {
        fprintf(stderr, "i3sd: configuration became obsolete during staging\n");
        generation_destroy(generation);
        app->reload_dirty = true;
        return NULL;
    }
    return generation;
}

void i3sd_fault_block(struct block *block) {
    if (block->faulted) {
        return;
    }
    block->faulted = true;
    struct generation *generation = block->generation;
    if (generation->app->spawn.active &&
        generation->app->spawn.block == block) {
        i3sd_spawn_cancel(generation->app);
    }
    for (struct logical_timer *timer = generation->timers; timer != NULL;
         timer = timer->next) {
        if (timer->block == block && !timer->cancelled) {
            i3sd_timer_cancel(&generation->app->timer_heap, &timer->timer);
            timer->cancelled = true;
        }
    }
    for (struct systemd_subscription *subscription =
             generation->systemd_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (subscription->block == block) {
            subscription->cancelled = true;
        }
    }
    for (struct power_profiles_subscription *subscription =
             generation->power_profiles_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (subscription->block == block) {
            subscription->cancelled = true;
        }
    }
    i3sd_dbus_cancel_owner(generation->dbus, block);
    block_state_destroy(&block->state);
    i3sd_buffer_clear(&block->fragment);
    generation->app->render_dirty = true;
}

static bool add_poll_timer(struct block *block, uint64_t activation_ns) {
    if (block->interval <= 0 || block->update_ref == LUA_NOREF) {
        return true;
    }
    uint64_t interval_ns;
    lua_State *lua = block->generation->lua;
    lua_pushnumber(lua, block->interval);
    bool valid = seconds_to_ns(lua, -1, false, &interval_ns);
    lua_pop(lua, 1);
    if (!valid) {
        return false;
    }
    struct logical_timer *timer = calloc(1, sizeof(*timer));
    if (timer == NULL) {
        return false;
    }
    timer->block = block;
    timer->callback_ref = block->update_ref;
    timer->delay_ns = interval_ns;
    timer->timer.interval_ns = interval_ns;
    timer->next = block->generation->timers;
    block->generation->timers = timer;
    block->generation->timer_count++;
    return i3sd_timer_add(&block->generation->app->timer_heap, &timer->timer,
                          activation_ns + interval_ns, interval_ns, timer);
}

static bool commit_generation(struct app *app, struct generation *candidate) {
    struct generation *old = app->current;
    app->current = candidate;
    candidate->staging = false;
    const uint64_t activation_ns = monotonic_now_ns();

    for (size_t index = 0; index < candidate->block_count; index++) {
        struct block *block = candidate->blocks[index];
        block_state_destroy(&block->state);
        i3sd_buffer_clear(&block->fragment);
    }
    for (struct logical_timer *timer = candidate->timers; timer != NULL;
         timer = timer->next) {
        const uint64_t interval = timer->timer.interval_ns;
        if (!i3sd_timer_add(&app->timer_heap, &timer->timer,
                            activation_ns + timer->delay_ns, interval, timer)) {
            fprintf(stderr, "i3sd: unable to activate configured timer\n");
            i3sd_fault_block(timer->block);
        }
    }
    for (size_t index = 0; index < candidate->block_count; index++) {
        struct block *block = candidate->blocks[index];
        if (!call_block_ref(block, block->update_ref, "live refresh", 0)) {
            i3sd_fault_block(block);
        } else if (!add_poll_timer(block, activation_ns)) {
            fprintf(stderr, "i3sd: unable to activate polling timer for %s\n",
                    block->name);
            i3sd_fault_block(block);
        }
    }
    generation_destroy(old);
    /* Reuse an authoritative shared-bus snapshot across generation reloads. */
    i3sd_systemd_notify(app);
    i3sd_power_profiles_notify(app);
    app->render_dirty = true;
    return true;
}

static bool render_status(struct app *app) {
    i3sd_buffer_clear(&app->frame);
    if (!i3sd_buffer_append_char(&app->frame, '[', I3SD_FRAME_MAX_BYTES)) {
        return false;
    }
    bool first = true;
    for (size_t index = 0; index < app->current->block_count; index++) {
        struct block *block = app->current->ordered[index];
        if (block->faulted || !block->state.visible) {
            continue;
        }
        if (!first &&
            !i3sd_buffer_append_char(&app->frame, ',', I3SD_FRAME_MAX_BYTES)) {
            return false;
        }
        if (!i3sd_buffer_append(&app->frame, block->fragment.data,
                                block->fragment.len, I3SD_FRAME_MAX_BYTES)) {
            return false;
        }
        first = false;
    }
    if (!i3sd_buffer_append_char(&app->frame, ']', I3SD_FRAME_MAX_BYTES)) {
        return false;
    }
    if (!i3sd_output_set_frame(&app->output, app->frame.data, app->frame.len)) {
        return false;
    }
    app->render_dirty = false;
    app->last_render_ns = monotonic_now_ns();
    return true;
}

static ssize_t fd_write(void *userdata, const void *data, size_t len) {
    const int fd = *(const int *)userdata;
    return write(fd, data, len);
}

static bool epoll_stdout(struct app *app, bool enabled) {
    if (enabled == app->stdout_registered) {
        return true;
    }
    struct epoll_event event = {
        .events = EPOLLOUT | EPOLLERR | EPOLLHUP,
        .data.u64 = SOURCE_STDOUT,
    };
    const int operation = enabled ? EPOLL_CTL_ADD : EPOLL_CTL_DEL;
    if (epoll_ctl(app->epoll_fd, operation, STDOUT_FILENO,
                  enabled ? &event : NULL) < 0 &&
        !(operation == EPOLL_CTL_DEL && errno == ENOENT)) {
        return false;
    }
    app->stdout_registered = enabled;
    return true;
}

static bool flush_output(struct app *app) {
    while (app->prelude_offset < sizeof(protocol_prelude) - 1) {
        ssize_t written =
            write(STDOUT_FILENO, protocol_prelude + app->prelude_offset,
                  sizeof(protocol_prelude) - 1 - app->prelude_offset);
        if (written > 0) {
            app->prelude_offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return epoll_stdout(app, true);
        }
        if (written < 0 && errno != EPIPE) {
            fprintf(stderr, "i3sd: stdout write failed: %s\n", strerror(errno));
        }
        app->running = false;
        return false;
    }
    int stdout_fd = STDOUT_FILENO;
    int error_number = 0;
    enum i3sd_flush_result result = i3sd_output_flush(
        &app->output, fd_write, &stdout_fd, I3SD_OUTPUT_BUDGET, &error_number);
    if (result == I3SD_FLUSH_CLOSED) {
        app->running = false;
        return false;
    }
    if (result == I3SD_FLUSH_ERROR) {
        fprintf(stderr, "i3sd: stdout write failed: %s\n",
                strerror(error_number));
        app->running = false;
        return false;
    }
    return epoll_stdout(app, i3sd_output_has_pending(&app->output));
}

static void dispatch_click(struct app *app, const char *json, size_t len) {
    yyjson_read_err error;
    yyjson_doc *document = yyjson_read_opts((char *)json, len, 0, NULL, &error);
    if (document == NULL) {
        fprintf(stderr, "i3sd: discarding malformed click event\n");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(document);
    yyjson_val *name_value = yyjson_obj_get(root, "name");
    yyjson_val *instance_value = yyjson_obj_get(root, "instance");
    yyjson_val *button_value = yyjson_obj_get(root, "button");
    if (!yyjson_is_obj(root) || !yyjson_is_str(name_value) ||
        !yyjson_is_str(instance_value) || !yyjson_is_int(button_value)) {
        yyjson_doc_free(document);
        return;
    }
    const char *name = yyjson_get_str(name_value);
    const char *instance = yyjson_get_str(instance_value);
    struct block *target = NULL;
    for (size_t index = 0; index < app->current->block_count; index++) {
        struct block *block = app->current->blocks[index];
        if (!block->faulted && strcmp(block->name, name) == 0 &&
            strcmp(block->token, instance) == 0) {
            target = block;
            break;
        }
    }
    if (target == NULL || target->click_ref == LUA_NOREF) {
        yyjson_doc_free(document);
        return;
    }

    lua_State *lua = target->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, target->click_ref);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, target->context_ref);
    lua_pushinteger(lua, yyjson_get_sint(button_value));
    lua_newtable(lua);
    static const char *const integer_fields[] = {
        "x",        "y",        "relative_x", "relative_y",
        "output_x", "output_y", "width",      "height",
    };
    for (size_t index = 0;
         index < sizeof(integer_fields) / sizeof(integer_fields[0]); index++) {
        yyjson_val *value = yyjson_obj_get(root, integer_fields[index]);
        if (yyjson_is_int(value)) {
            lua_pushinteger(lua, yyjson_get_sint(value));
            lua_setfield(lua, -2, integer_fields[index]);
        }
    }
    yyjson_val *modifiers = yyjson_obj_get(root, "modifiers");
    if (yyjson_is_arr(modifiers)) {
        lua_newtable(lua);
        size_t index, maximum;
        yyjson_val *value;
        yyjson_arr_foreach(modifiers, index, maximum, value) {
            if (yyjson_is_str(value)) {
                lua_pushstring(lua, yyjson_get_str(value));
                lua_rawseti(lua, -2, (int)index + 1);
            }
        }
        lua_setfield(lua, -2, "modifiers");
    }
    if (lua_pcall(lua, 3, 0, 0) != 0) {
        i3sd_log_lua_error(target, "click");
        i3sd_fault_block(target);
    }
    yyjson_doc_free(document);
}

static void read_clicks(struct app *app) {
    char bytes[16384];
    for (;;) {
        ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count == 0) {
            app->running = false;
            return;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                app->running = false;
            }
            return;
        }
        size_t offset = 0;
        while (offset < (size_t)count) {
            size_t consumed, event_len;
            const char *event;
            enum i3sd_click_result result = i3sd_click_framer_feed(
                &app->click_framer, bytes + offset, (size_t)count - offset,
                &consumed, &event, &event_len);
            offset += consumed;
            if (result == I3SD_CLICK_EVENT) {
                dispatch_click(app, event, event_len);
            } else if (result == I3SD_CLICK_FATAL_FRAMING) {
                fprintf(stderr,
                        "i3sd: click stream cannot be resynchronized\n");
                return;
            }
        }
    }
}

static void handle_signals(struct app *app) {
    struct signalfd_siginfo info[16];
    for (;;) {
        ssize_t count = read(app->signal_fd, info, sizeof(info));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (count == 0) {
            return;
        }
        const size_t signals = (size_t)count / sizeof(info[0]);
        for (size_t index = 0; index < signals; index++) {
            switch (info[index].ssi_signo) {
            case SIGINT:
            case SIGTERM:
            case SIGPIPE:
                app->running = false;
                break;
            case SIGHUP:
                app->reload_dirty = true;
                break;
            case SIGCONT:
                for (size_t block_index = 0;
                     block_index < app->current->block_count; block_index++) {
                    struct block *block = app->current->blocks[block_index];
                    if (!call_block_ref(block, block->update_ref,
                                        "resume refresh", 0)) {
                        i3sd_fault_block(block);
                    }
                }
                break;
            case SIGCHLD: {
                pid_t child;
                int status;
                do {
                    child = waitpid(-1, &status, WNOHANG);
                    if (app->spawn.active && child == app->spawn.pid) {
                        app->spawn.pid = 0;
                        app->spawn.wait_status = status;
                        app->spawn.exited = true;
                        i3sd_spawn_finish(app);
                    }
                } while (child > 0);
                break;
            }
            default:
                break;
            }
        }
    }
}

static void handle_inotify(struct app *app) {
    char bytes[16384];
    for (;;) {
        ssize_t count = read(app->inotify_fd, bytes, sizeof(bytes));
        if (count <= 0) {
            return;
        }
        size_t offset = 0;
        while (offset < (size_t)count) {
            struct inotify_event *event =
                (struct inotify_event *)(bytes + offset);
            if ((event->mask & IN_Q_OVERFLOW) != 0 ||
                (event->len > 0 &&
                 strcmp(event->name, app->config_base) == 0)) {
                app->reload_dirty = true;
            }
            offset += sizeof(*event) + event->len;
        }
    }
}

static void dispatch_timers(struct app *app, uint64_t now_ns) {
    for (size_t count = 0; count < I3SD_TIMER_BUDGET; count++) {
        struct i3sd_timer *native =
            i3sd_timer_pop_due(&app->timer_heap, now_ns);
        if (native == NULL) {
            return;
        }
        struct logical_timer *timer = native->userdata;
        if (timer->cancelled || timer->block->generation != app->current ||
            timer->block->faulted) {
            continue;
        }
        if (!call_block_ref(timer->block, timer->callback_ref, "timer", 0)) {
            i3sd_fault_block(timer->block);
            continue;
        }
        if (native->interval_ns != 0 && !timer->cancelled &&
            !timer->block->faulted &&
            !i3sd_timer_reschedule_fixed(&app->timer_heap, native, now_ns)) {
            fprintf(stderr, "i3sd: repeating timer overflow\n");
            i3sd_fault_block(timer->block);
        }
    }
}

static int epoll_timeout_ms(struct app *app, uint64_t now_ns) {
    uint64_t deadline = UINT64_MAX;
    struct i3sd_timer *timer = i3sd_timer_peek(&app->timer_heap);
    if (timer != NULL) {
        deadline = timer->deadline_ns;
    }
    if (app->render_dirty && app->last_render_ns != 0) {
        const uint64_t render_deadline =
            app->last_render_ns + I3SD_RENDER_INTERVAL_NS;
        if (render_deadline < deadline) {
            deadline = render_deadline;
        }
    }
    for (size_t index = 0; index < 2; index++) {
        struct systemd_bus *source = &app->systemd_buses[index];
        if (!i3sd_systemd_bus_needed(app, source->scope)) {
            continue;
        }
        if (source->bus == NULL) {
            if (source->retry_deadline_ns != 0 &&
                source->retry_deadline_ns < deadline) {
                deadline = source->retry_deadline_ns;
            }
            continue;
        }
        uint64_t timeout_us;
        if (sd_bus_get_timeout(source->bus, &timeout_us) >= 0 &&
            timeout_us != UINT64_MAX) {
            const uint64_t timeout_ns =
                timeout_us > UINT64_MAX / 1000 ? UINT64_MAX : timeout_us * 1000;
            if (timeout_ns < deadline) {
                deadline = timeout_ns;
            }
        }
    }
    struct power_profiles_source *power = &app->power_profiles;
    if (i3sd_power_profiles_needed(app)) {
        if (power->bus == NULL) {
            if (power->retry_deadline_ns != 0 &&
                power->retry_deadline_ns < deadline) {
                deadline = power->retry_deadline_ns;
            }
        } else {
            uint64_t timeout_us;
            if (sd_bus_get_timeout(power->bus, &timeout_us) >= 0 &&
                timeout_us != UINT64_MAX) {
                const uint64_t timeout_ns = timeout_us > UINT64_MAX / 1000
                                                ? UINT64_MAX
                                                : timeout_us * 1000;
                if (timeout_ns < deadline) {
                    deadline = timeout_ns;
                }
            }
        }
    }
    const uint64_t dbus_deadline = i3sd_dbus_deadline(
        app->dbus, app->current == NULL ? NULL : app->current->dbus);
    if (dbus_deadline < deadline) {
        deadline = dbus_deadline;
    }
    if (app->reload_dirty || (app->render_dirty && app->last_render_ns == 0)) {
        return 0;
    }
    if (deadline == UINT64_MAX) {
        return -1;
    }
    if (deadline <= now_ns) {
        return 0;
    }
    uint64_t milliseconds = (deadline - now_ns + 999999ULL) / 1000000ULL;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static bool add_epoll_fd(struct app *app, int fd, uint32_t events,
                         uint64_t cookie) {
    struct epoll_event event = {.events = events, .data.u64 = cookie};
    return epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static bool make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool initialize_runtime(struct app *app, const sigset_t *signal_mask) {
    /* Dynamic source cookies start above the fixed reactor source IDs. */
    app->next_registration_cookie = SOURCE_FIXED_MAX;
    app->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    app->signal_fd = signalfd(-1, signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    app->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (app->epoll_fd < 0 || app->signal_fd < 0 || app->inotify_fd < 0 ||
        !make_nonblocking(STDIN_FILENO) || !make_nonblocking(STDOUT_FILENO)) {
        return false;
    }
    app->config_watch =
        inotify_add_watch(app->inotify_fd, app->config_dir,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                              IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
    if (app->config_watch < 0) {
        return false;
    }
    app->systemd_buses[0] = (struct systemd_bus){
        .app = app,
        .cookie = SOURCE_SYSTEM_BUS,
        .registered_fd = -1,
        .scope = SYSTEMD_SCOPE_SYSTEM,
    };
    app->systemd_buses[1] = (struct systemd_bus){
        .app = app,
        .cookie = SOURCE_USER_BUS,
        .registered_fd = -1,
        .scope = SYSTEMD_SCOPE_USER,
    };
    app->power_profiles = (struct power_profiles_source){
        .app = app,
        .registered_fd = -1,
    };
    app->dbus =
        i3sd_dbus_runtime_create(app->epoll_fd, &app->next_registration_cookie);
    if (app->dbus == NULL) {
        return false;
    }
    app->spawn.output_fd = -1;
    app->spawn.callback_ref = LUA_NOREF;
    return add_epoll_fd(app, app->signal_fd, EPOLLIN, SOURCE_SIGNAL) &&
           add_epoll_fd(app, STDIN_FILENO, EPOLLIN | EPOLLERR | EPOLLHUP,
                        SOURCE_STDIN) &&
           add_epoll_fd(app, app->inotify_fd, EPOLLIN, SOURCE_INOTIFY) &&
           epoll_stdout(app, true);
}

static void run_event_loop(struct app *app) {
    struct epoll_event events[I3SD_EPOLL_EVENTS];
    app->running = true;
    while (app->running) {
        uint64_t now_ns = monotonic_now_ns();
        i3sd_systemd_reconcile(&app->systemd_buses[0], now_ns);
        i3sd_systemd_reconcile(&app->systemd_buses[1], now_ns);
        i3sd_power_profiles_reconcile(&app->power_profiles, now_ns);
        i3sd_dbus_reconcile(app->dbus, app->current->dbus, now_ns);
        int event_count;
        do {
            event_count = epoll_wait(app->epoll_fd, events, I3SD_EPOLL_EVENTS,
                                     epoll_timeout_ms(app, now_ns));
        } while (event_count < 0 && errno == EINTR);
        if (event_count < 0) {
            fprintf(stderr, "i3sd: epoll_wait failed: %s\n", strerror(errno));
            break;
        }
        for (int index = 0; index < event_count; index++) {
            switch (events[index].data.u64) {
            case SOURCE_SIGNAL:
                handle_signals(app);
                break;
            case SOURCE_STDIN:
                read_clicks(app);
                break;
            case SOURCE_STDOUT:
                flush_output(app);
                break;
            case SOURCE_INOTIFY:
                handle_inotify(app);
                break;
            case SOURCE_SYSTEM_BUS:
                i3sd_systemd_process(&app->systemd_buses[0],
                                     monotonic_now_ns());
                break;
            case SOURCE_USER_BUS:
                i3sd_systemd_process(&app->systemd_buses[1],
                                     monotonic_now_ns());
                break;
            case SOURCE_POWER_PROFILES:
                i3sd_power_profiles_process(&app->power_profiles,
                                            monotonic_now_ns());
                break;
            case SOURCE_SPAWN:
                i3sd_spawn_read(app);
                break;
            default:
                i3sd_dbus_process_cookie(app->dbus, app->current->dbus,
                                         events[index].data.u64,
                                         monotonic_now_ns());
                break;
            }
        }

        now_ns = monotonic_now_ns();
        i3sd_systemd_process(&app->systemd_buses[0], now_ns);
        i3sd_systemd_process(&app->systemd_buses[1], now_ns);
        i3sd_power_profiles_process(&app->power_profiles, now_ns);
        i3sd_dbus_process(app->dbus, app->current->dbus, now_ns);
        dispatch_timers(app, now_ns);
        if (app->reload_dirty) {
            app->reload_dirty = false;
            struct generation *candidate = stage_generation(app);
            if (candidate != NULL) {
                commit_generation(app, candidate);
            }
        }
        if (app->render_dirty && !i3sd_output_has_pending(&app->output) &&
            (app->last_render_ns == 0 ||
             now_ns - app->last_render_ns >= I3SD_RENDER_INTERVAL_NS)) {
            if (!render_status(app)) {
                fprintf(stderr, "i3sd: fatal render failure\n");
                break;
            }
        }
        if (app->prelude_offset < sizeof(protocol_prelude) - 1 ||
            i3sd_output_has_pending(&app->output)) {
            flush_output(app);
        }
    }
}

static void app_destroy(struct app *app) {
    if (app->dbus != NULL) {
        i3sd_dbus_reconcile(app->dbus, NULL, monotonic_now_ns());
    }
    generation_destroy(app->current);
    app->current = NULL;
    if (app->systemd_buses[0].bus != NULL) {
        i3sd_systemd_close(&app->systemd_buses[0]);
    }
    if (app->systemd_buses[1].bus != NULL) {
        i3sd_systemd_close(&app->systemd_buses[1]);
    }
    if (app->power_profiles.bus != NULL) {
        i3sd_power_profiles_close(&app->power_profiles);
    }
    i3sd_dbus_runtime_destroy(app->dbus);
    if (app->spawn.active) {
        i3sd_spawn_cancel(app);
    }
    i3sd_buffer_destroy(&app->spawn.output);
    i3sd_timer_heap_destroy(&app->timer_heap);
    i3sd_output_destroy(&app->output);
    i3sd_click_framer_destroy(&app->click_framer);
    i3sd_buffer_destroy(&app->frame);
    if (app->signal_fd >= 0) {
        close(app->signal_fd);
    }
    if (app->inotify_fd >= 0) {
        close(app->inotify_fd);
    }
    if (app->epoll_fd >= 0) {
        close(app->epoll_fd);
    }
    for (size_t index = 0; index < app->identity_count; index++) {
        free(app->identities[index].name);
        free(app->identities[index].key);
    }
    free(app->config_path);
    free(app->config_dir);
    free(app->config_base);
}

static char *default_config_path(void) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home == NULL || config_home[0] == '\0') {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            return NULL;
        }
        size_t length = strlen(home) + strlen("/.config/i3sd.lua") + 1;
        char *path = malloc(length);
        if (path != NULL) {
            snprintf(path, length, "%s/.config/i3sd.lua", home);
        }
        return path;
    }
    size_t length = strlen(config_home) + strlen("/i3sd.lua") + 1;
    char *path = malloc(length);
    if (path != NULL) {
        snprintf(path, length, "%s/i3sd.lua", config_home);
    }
    return path;
}

static bool split_config_path(struct app *app) {
    char *slash = strrchr(app->config_path, '/');
    if (slash == NULL) {
        app->config_dir = strdup(".");
        app->config_base = strdup(app->config_path);
    } else {
        size_t directory_len = (size_t)(slash - app->config_path);
        app->config_dir = copy_bytes(app->config_path,
                                     directory_len == 0 ? 1 : directory_len);
        if (directory_len == 0) {
            app->config_dir[0] = '/';
        }
        app->config_base = strdup(slash + 1);
    }
    return app->config_dir != NULL && app->config_base != NULL;
}

static void print_help(FILE *stream) {
    fprintf(stream,
            "Usage: i3sd [OPTIONS]\n"
            "  -c, --config FILE  configuration file\n"
            "      --check        validate configuration without output\n"
            "      --debug        enable lifecycle diagnostics\n"
            "      --version      print version\n"
            "  -h, --help         show this help\n");
}

int main(int argc, char **argv) {
    /* Install the process signal mask before LuaJIT or optional libraries. */
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGHUP);
    sigaddset(&signal_mask, SIGCONT);
    sigaddset(&signal_mask, SIGPIPE);
    sigaddset(&signal_mask, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) < 0) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    struct app app = {
        .epoll_fd = -1,
        .signal_fd = -1,
        .inotify_fd = -1,
        .config_watch = -1,
    };
    bool check_only = false;
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check", no_argument, NULL, 1000},
        {"debug", no_argument, NULL, 1001},
        {"version", no_argument, NULL, 1002},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;
    while ((option = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
        switch (option) {
        case 'c':
            free(app.config_path);
            app.config_path = strdup(optarg);
            break;
        case 'h':
            print_help(stdout);
            app_destroy(&app);
            return EXIT_SUCCESS;
        case 1000:
            check_only = true;
            break;
        case 1001:
            app.debug = true;
            break;
        case 1002:
            printf("i3sd %s\n", I3SD_VERSION);
            app_destroy(&app);
            return EXIT_SUCCESS;
        default:
            print_help(stderr);
            app_destroy(&app);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        print_help(stderr);
        app_destroy(&app);
        return EXIT_FAILURE;
    }
    if (app.config_path == NULL) {
        app.config_path = default_config_path();
    }
    if (app.config_path == NULL || !split_config_path(&app)) {
        fprintf(stderr, "i3sd: unable to determine configuration path\n");
        app_destroy(&app);
        return EXIT_FAILURE;
    }

    i3sd_output_init(&app.output);
    struct generation *initial = stage_generation(&app);
    if (initial == NULL) {
        app_destroy(&app);
        return EXIT_FAILURE;
    }
    if (check_only) {
        generation_destroy(initial);
        app_destroy(&app);
        return EXIT_SUCCESS;
    }
    if (!initialize_runtime(&app, &signal_mask)) {
        fprintf(stderr, "i3sd: runtime initialization failed: %s\n",
                strerror(errno));
        generation_destroy(initial);
        app_destroy(&app);
        return EXIT_FAILURE;
    }
    commit_generation(&app, initial);
    run_event_loop(&app);
    app_destroy(&app);
    return EXIT_SUCCESS;
}
