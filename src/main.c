#define _GNU_SOURCE

#include "config.h"
#include "i3sd/buffer.h"
#include "i3sd/click.h"
#include "i3sd/output.h"
#include "i3sd/timer.h"
#include "i3sd/utf8.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <systemd/sd-bus.h>
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
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define I3SD_MAX_BLOCKS 256U
#define I3SD_MAX_TIMERS 4096U
#define I3SD_MAX_TEXT 6144U
#define I3SD_MAX_BLOCK_JSON 7168U
#define I3SD_MAX_CONFIG (1024U * 1024U)
#define I3SD_EPOLL_EVENTS 64
#define I3SD_TIMER_BUDGET 256U
#define I3SD_OUTPUT_BUDGET (1024U * 1024U)
#define I3SD_RENDER_INTERVAL_NS 50000000ULL
#define I3SD_ROFI_CHOICE_LIMIT 256U
#define I3SD_ROFI_CHOICE_BYTES 1024U
#define I3SD_ROFI_INPUT_BYTES (64U * 1024U)
#define I3SD_ROFI_PROMPT_BYTES 256U

enum source_cookie {
    SOURCE_SIGNAL = 1,
    SOURCE_STDIN = 2,
    SOURCE_STDOUT = 3,
    SOURCE_INOTIFY = 4,
    SOURCE_SYSTEM_BUS = 5,
    SOURCE_USER_BUS = 6,
    SOURCE_POWER_PROFILES = 7,
    SOURCE_ROFI = 8,
};

struct app;
struct generation;
struct block;
struct systemd_subscription;
struct power_profiles_subscription;

#define I3SD_POWER_PROFILE_LIMIT 16U
#define I3SD_POWER_PROFILE_NAME_LIMIT 63U

enum systemd_scope {
    SYSTEMD_SCOPE_SYSTEM,
    SYSTEMD_SCOPE_USER,
    SYSTEMD_SCOPE_BOTH,
};

struct systemd_bus {
    struct app *app;
    sd_bus *bus;
    sd_bus_slot *match_slot;
    uint64_t cookie;
    uint64_t retry_deadline_ns;
    uint32_t failed_count;
    int registered_fd;
    uint32_t registered_events;
    enum systemd_scope scope;
    bool count_valid;
    bool query_inflight;
    bool subscribe_inflight;
};

struct block_state {
    char *full_text;
    char *short_text;
    char *color;
    char *background;
    char *border;
    char *min_width_string;
    int64_t min_width_integer;
    char *align;
    char *markup;
    int border_top;
    int border_right;
    int border_bottom;
    int border_left;
    bool min_width_is_string;
    bool min_width_present;
    bool urgent;
    bool visible;
};

struct logical_timer {
    struct i3sd_timer timer;
    struct logical_timer *next;
    struct block *block;
    int callback_ref;
    uint64_t delay_ns;
    bool cancelled;
};

struct systemd_subscription {
    struct systemd_subscription *next;
    struct block *block;
    int callback_ref;
    enum systemd_scope scope;
    bool cancelled;
};

struct power_profiles_subscription {
    struct power_profiles_subscription *next;
    struct block *block;
    int callback_ref;
    bool cancelled;
};

struct power_profiles_source {
    struct app *app;
    sd_bus *bus;
    sd_bus_slot *match_slot;
    uint64_t retry_deadline_ns;
    char active[I3SD_POWER_PROFILE_NAME_LIMIT + 1];
    char profiles[I3SD_POWER_PROFILE_LIMIT][I3SD_POWER_PROFILE_NAME_LIMIT + 1];
    size_t profile_count;
    int registered_fd;
    uint32_t registered_events;
    bool active_valid;
    bool profiles_valid;
    bool active_query_inflight;
    bool profiles_query_inflight;
};

struct rofi_menu {
    pid_t pid;
    int output_fd;
    struct block *block;
    int callback_ref;
    char result[I3SD_ROFI_CHOICE_BYTES + 2];
    size_t result_len;
};

struct block {
    struct generation *generation;
    char *name;
    char *key;
    bool has_key;
    char token[33];
    int order;
    size_t declaration_order;
    double interval;
    int init_ref;
    int update_ref;
    int click_ref;
    int context_ref;
    bool faulted;
    struct block_state state;
    struct i3sd_buffer fragment;
};

struct generation {
    struct app *app;
    lua_State *lua;
    struct block *blocks[I3SD_MAX_BLOCKS];
    struct block *ordered[I3SD_MAX_BLOCKS];
    size_t block_count;
    struct logical_timer *timers;
    struct systemd_subscription *systemd_subscriptions;
    struct power_profiles_subscription *power_profiles_subscriptions;
    size_t timer_count;
    size_t subscription_count;
    int push_uint64_ref;
    int push_int64_ref;
    bool staging;
};

struct identity {
    char *name;
    char *key;
    bool has_key;
    char token[33];
};

struct app {
    int epoll_fd;
    int signal_fd;
    int inotify_fd;
    int config_watch;
    char *config_path;
    char *config_dir;
    char *config_base;
    struct generation *current;
    struct i3sd_timer_heap timer_heap;
    struct i3sd_output output;
    struct i3sd_click_framer click_framer;
    struct systemd_bus systemd_buses[2];
    struct power_profiles_source power_profiles;
    struct rofi_menu rofi;
    struct i3sd_buffer frame;
    struct identity identities[4096];
    size_t identity_count;
    size_t prelude_offset;
    uint64_t last_render_ns;
    bool stdout_registered;
    bool render_dirty;
    bool reload_dirty;
    bool running;
    bool debug;
};

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

static const char protocol_prelude[] =
    "{\"version\":1,\"click_events\":true}\n[\n";
static const char context_metatable[] = "i3sd.context";
static const char timer_metatable[] = "i3sd.timer";
static const char systemd_handle_metatable[] = "i3sd.systemd_handle";
static const char power_profiles_handle_metatable[] =
    "i3sd.power_profiles_handle";

static void fault_block(struct block *block);
static bool open_rofi_menu(struct app *app, struct block *block,
                           const char *prompt, const char *choices,
                           size_t choices_len, int callback_ref);
static void cancel_rofi_menu(struct app *app,
                             const struct generation *generation);
static bool set_power_profile(struct app *app, const char *profile);
static bool known_power_profile(const struct power_profiles_source *source,
                                const char *profile);
static bool make_nonblocking(int fd);

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

static void log_lua_error(struct block *block, const char *phase) {
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

static int lua_context_rofi(lua_State *lua) {
    struct lua_context *context = check_context(lua, 1);
    struct block *block = context->block;
    luaL_checktype(lua, 2, LUA_TTABLE);
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    static const char *const fields[] = {"prompt", "choices"};
    check_strict_table(lua, 2, fields, sizeof(fields) / sizeof(fields[0]));

    lua_getfield(lua, 2, "prompt");
    const char *prompt = "Select";
    if (!lua_isnil(lua, -1)) {
        luaL_checktype(lua, -1, LUA_TSTRING);
        size_t prompt_len;
        prompt = luaL_checklstring(lua, -1, &prompt_len);
        if (!valid_text(prompt, prompt_len, I3SD_ROFI_PROMPT_BYTES) ||
            memchr(prompt, '\n', prompt_len) != NULL ||
            memchr(prompt, '\r', prompt_len) != NULL) {
            return luaL_error(lua,
                              "rofi prompt must be one line of valid "
                              "UTF-8 up to %u bytes",
                              I3SD_ROFI_PROMPT_BYTES);
        }
    }

    lua_getfield(lua, 2, "choices");
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int choices_index = lua_gettop(lua);
    const size_t choice_count = lua_objlen(lua, choices_index);
    if (choice_count == 0 || choice_count > I3SD_ROFI_CHOICE_LIMIT) {
        return luaL_error(lua, "rofi choices must contain 1 to %u entries",
                          I3SD_ROFI_CHOICE_LIMIT);
    }

    /* Reject sparse or keyed tables so menu order is fully deterministic. */
    size_t encountered = 0;
    lua_pushnil(lua);
    while (lua_next(lua, choices_index) != 0) {
        if (lua_type(lua, -2) != LUA_TNUMBER) {
            return luaL_error(lua,
                              "rofi choices must be a dense 1-based sequence");
        }
        const lua_Number numeric_key = lua_tonumber(lua, -2);
        if (!isfinite(numeric_key) || numeric_key < 1 ||
            numeric_key > (lua_Number)choice_count ||
            floor(numeric_key) != numeric_key) {
            return luaL_error(lua,
                              "rofi choices must be a dense 1-based sequence");
        }
        encountered++;
        lua_pop(lua, 1);
    }
    if (encountered != choice_count) {
        return luaL_error(lua, "rofi choices must be a dense 1-based sequence");
    }

    for (size_t index = 1; index <= choice_count; index++) {
        lua_rawgeti(lua, choices_index, (int)index);
        if (lua_type(lua, -1) != LUA_TSTRING) {
            return luaL_error(lua, "rofi choice %zu must be a string", index);
        }
        size_t choice_len;
        const char *choice = luaL_checklstring(lua, -1, &choice_len);
        if (choice_len == 0 ||
            !valid_text(choice, choice_len, I3SD_ROFI_CHOICE_BYTES) ||
            memchr(choice, '\n', choice_len) != NULL ||
            memchr(choice, '\r', choice_len) != NULL) {
            return luaL_error(lua,
                              "rofi choice %zu must be one non-empty line "
                              "of valid UTF-8 up to %u bytes",
                              index, I3SD_ROFI_CHOICE_BYTES);
        }
        lua_pop(lua, 1);
    }

    struct i3sd_buffer input = {0};
    for (size_t index = 1; index <= choice_count; index++) {
        lua_rawgeti(lua, choices_index, (int)index);
        size_t choice_len;
        const char *choice = lua_tolstring(lua, -1, &choice_len);
        const bool appended =
            i3sd_buffer_append(&input, choice, choice_len,
                               I3SD_ROFI_INPUT_BYTES) &&
            i3sd_buffer_append_char(&input, '\n', I3SD_ROFI_INPUT_BYTES);
        lua_pop(lua, 1);
        if (!appended) {
            i3sd_buffer_destroy(&input);
            return luaL_error(lua, "rofi choices exceed the %u-byte limit",
                              I3SD_ROFI_INPUT_BYTES);
        }
    }

    struct app *app = block->generation->app;
    if (app->current != block->generation || block->faulted) {
        i3sd_buffer_destroy(&input);
        lua_pushboolean(lua, false);
        return 1;
    }

    lua_pushvalue(lua, 3);
    const int callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    const bool opened =
        open_rofi_menu(app, block, prompt, input.data, input.len, callback_ref);
    i3sd_buffer_destroy(&input);
    if (!opened) {
        luaL_unref(lua, LUA_REGISTRYINDEX, callback_ref);
    }
    lua_pushboolean(lua, opened);
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
    const bool accepted = current && app->power_profiles.profiles_valid &&
                          known_power_profile(&app->power_profiles, profile) &&
                          set_power_profile(app, profile);
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
        strcmp(feature, "power_profiles") == 0 || strcmp(feature, "rofi") == 0;
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
    lua_setfield(lua, -2, "rofi");
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
        log_lua_error(block, phase);
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

static void register_lua_api(struct generation *generation) {
    lua_State *lua = generation->lua;
    luaL_newmetatable(lua, context_metatable);
    lua_newtable(lua);
    static const luaL_Reg context_methods[] = {
        {"set", lua_context_set},
        {"after", lua_context_after},
        {"every", lua_context_every},
        {"sample", lua_context_sample},
        {"rofi", lua_context_rofi},
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

    lua_newtable(lua);
    lua_pushinteger(lua, 1);
    lua_setfield(lua, -2, "api_version");
    lua_pushstring(lua, I3SD_VERSION);
    lua_setfield(lua, -2, "version");
    lua_pushcfunction(lua, lua_has_feature);
    lua_setfield(lua, -2, "has_feature");
    lua_pushcfunction(lua, lua_features);
    lua_setfield(lua, -2, "features");
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

static bool systemd_scope_uses(enum systemd_scope subscription_scope,
                               enum systemd_scope bus_scope) {
    return subscription_scope == SYSTEMD_SCOPE_BOTH ||
           subscription_scope == bus_scope;
}

static bool systemd_bus_needed(const struct app *app,
                               enum systemd_scope scope) {
    if (app->current == NULL) {
        return false;
    }
    for (struct systemd_subscription *subscription =
             app->current->systemd_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (!subscription->cancelled &&
            systemd_scope_uses(subscription->scope, scope)) {
            return true;
        }
    }
    return false;
}

static void notify_systemd_subscribers(struct app *app) {
    struct generation *generation = app->current;
    if (generation == NULL || generation->staging) {
        return;
    }
    struct systemd_bus *system_bus = &app->systemd_buses[0];
    struct systemd_bus *user_bus = &app->systemd_buses[1];
    for (struct systemd_subscription *subscription =
             generation->systemd_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        struct block *block = subscription->block;
        if (subscription->cancelled || block->faulted) {
            continue;
        }
        if ((systemd_scope_uses(subscription->scope, SYSTEMD_SCOPE_SYSTEM) &&
             !system_bus->count_valid) ||
            (systemd_scope_uses(subscription->scope, SYSTEMD_SCOPE_USER) &&
             !user_bus->count_valid)) {
            continue;
        }

        uint32_t total = 0;
        lua_State *lua = generation->lua;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, subscription->callback_ref);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
        lua_newtable(lua);
        if (systemd_scope_uses(subscription->scope, SYSTEMD_SCOPE_SYSTEM)) {
            total += system_bus->failed_count;
            lua_pushinteger(lua, system_bus->failed_count);
            lua_setfield(lua, -2, "system_count");
        }
        if (systemd_scope_uses(subscription->scope, SYSTEMD_SCOPE_USER)) {
            total += user_bus->failed_count;
            lua_pushinteger(lua, user_bus->failed_count);
            lua_setfield(lua, -2, "user_count");
        }
        lua_pushinteger(lua, total);
        lua_setfield(lua, -2, "count");
        if (lua_pcall(lua, 2, 0, 0) != 0) {
            log_lua_error(block, "systemd event");
            fault_block(block);
        }
    }
}

static int systemd_snapshot_reply(sd_bus_message *message, void *userdata,
                                  sd_bus_error *ret_error) {
    (void)ret_error;
    struct systemd_bus *source = userdata;
    source->query_inflight = false;
    if (sd_bus_message_is_method_error(message, NULL)) {
        source->count_valid = false;
        return 0;
    }
    uint32_t count;
    int result =
        sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "u");
    if (result >= 0) {
        result = sd_bus_message_read(message, "u", &count);
    }
    if (result < 0) {
        source->count_valid = false;
        return 0;
    }
    const bool changed = !source->count_valid || source->failed_count != count;
    source->failed_count = count;
    source->count_valid = true;
    if (changed) {
        notify_systemd_subscribers(source->app);
    }
    return 0;
}

static void request_systemd_snapshot(struct systemd_bus *source) {
    if (source->bus == NULL || source->query_inflight) {
        return;
    }
    int result = sd_bus_call_method_async(
        source->bus, NULL, "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1", "org.freedesktop.DBus.Properties", "Get",
        systemd_snapshot_reply, source, "ss",
        "org.freedesktop.systemd1.Manager", "NFailedUnits");
    if (result >= 0) {
        source->query_inflight = true;
    }
}

static int systemd_property_changed(sd_bus_message *message, void *userdata,
                                    sd_bus_error *ret_error) {
    (void)message;
    (void)ret_error;
    request_systemd_snapshot(userdata);
    return 0;
}

static int systemd_subscribe_reply(sd_bus_message *message, void *userdata,
                                   sd_bus_error *ret_error) {
    (void)ret_error;
    struct systemd_bus *source = userdata;
    source->subscribe_inflight = false;
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *error = sd_bus_message_get_error(message);
        if (error == NULL || error->name == NULL ||
            strcmp(error->name, "org.freedesktop.systemd1.AlreadySubscribed") !=
                0) {
            source->count_valid = false;
            return 0;
        }
    }
    request_systemd_snapshot(source);
    return 0;
}

static void request_systemd_subscribe(struct systemd_bus *source) {
    if (source->bus == NULL || source->subscribe_inflight) {
        return;
    }
    int result = sd_bus_call_method_async(
        source->bus, NULL, "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
        "Subscribe", systemd_subscribe_reply, source, NULL);
    if (result >= 0) {
        source->subscribe_inflight = true;
    }
}

static int systemd_match_installed(sd_bus_message *message, void *userdata,
                                   sd_bus_error *ret_error) {
    (void)ret_error;
    struct systemd_bus *source = userdata;
    if (!sd_bus_message_is_method_error(message, NULL)) {
        request_systemd_subscribe(source);
    }
    return 0;
}

static void close_systemd_bus(struct systemd_bus *source) {
    if (source->registered_fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->registered_fd,
                  NULL);
    }
    source->registered_fd = -1;
    source->registered_events = 0;
    source->match_slot = sd_bus_slot_unref(source->match_slot);
    source->bus = sd_bus_flush_close_unref(source->bus);
    source->count_valid = false;
    source->query_inflight = false;
    source->subscribe_inflight = false;
}

static bool open_systemd_bus(struct systemd_bus *source, uint64_t now_ns) {
    int result = source->scope == SYSTEMD_SCOPE_SYSTEM
                     ? sd_bus_open_system(&source->bus)
                     : sd_bus_open_user(&source->bus);
    if (result < 0) {
        source->bus = NULL;
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    sd_bus_set_exit_on_disconnect(source->bus, 0);
    static const char match[] =
        "type='signal',sender='org.freedesktop.systemd1',"
        "path='/org/freedesktop/systemd1',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "arg0='org.freedesktop.systemd1.Manager'";
    result = sd_bus_add_match_async(source->bus, &source->match_slot, match,
                                    systemd_property_changed,
                                    systemd_match_installed, source);
    if (result < 0) {
        close_systemd_bus(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->retry_deadline_ns = 0;
    return true;
}

static bool reconcile_systemd_bus(struct systemd_bus *source, uint64_t now_ns) {
    if (!systemd_bus_needed(source->app, source->scope)) {
        if (source->bus != NULL) {
            close_systemd_bus(source);
        }
        return true;
    }
    if (source->bus == NULL) {
        if (source->retry_deadline_ns > now_ns) {
            return true;
        }
        if (!open_systemd_bus(source, now_ns)) {
            return true;
        }
    }

    const int fd = sd_bus_get_fd(source->bus);
    const int poll_events = sd_bus_get_events(source->bus);
    if (fd < 0 || poll_events < 0) {
        close_systemd_bus(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return true;
    }
    uint32_t events = EPOLLERR | EPOLLHUP;
    if ((poll_events & POLLIN) != 0) {
        events |= EPOLLIN;
    }
    if ((poll_events & POLLOUT) != 0) {
        events |= EPOLLOUT;
    }
    if (source->registered_fd == fd && source->registered_events == events) {
        return true;
    }
    if (source->registered_fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->registered_fd,
                  NULL);
        source->registered_fd = -1;
    }
    struct epoll_event event = {.events = events, .data.u64 = source->cookie};
    if (epoll_ctl(source->app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        close_systemd_bus(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->registered_fd = fd;
    source->registered_events = events;
    return true;
}

static void process_systemd_bus(struct systemd_bus *source, uint64_t now_ns) {
    if (source->bus == NULL) {
        return;
    }
    for (size_t count = 0; count < 256; count++) {
        int result = sd_bus_process(source->bus, NULL);
        if (result > 0) {
            continue;
        }
        if (result < 0) {
            close_systemd_bus(source);
            source->retry_deadline_ns = now_ns + 5000000000ULL;
        }
        break;
    }
    reconcile_systemd_bus(source, now_ns);
}

static bool power_profiles_needed(const struct app *app) {
    if (app->current == NULL) {
        return false;
    }
    for (struct power_profiles_subscription *subscription =
             app->current->power_profiles_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (!subscription->cancelled) {
            return true;
        }
    }
    return false;
}

static void notify_power_profiles_subscribers(struct app *app) {
    struct generation *generation = app->current;
    struct power_profiles_source *source = &app->power_profiles;
    if (generation == NULL || generation->staging || !source->active_valid ||
        !source->profiles_valid) {
        return;
    }
    for (struct power_profiles_subscription *subscription =
             generation->power_profiles_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        struct block *block = subscription->block;
        if (subscription->cancelled || block->faulted) {
            continue;
        }
        lua_State *lua = generation->lua;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, subscription->callback_ref);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
        lua_newtable(lua);
        lua_pushstring(lua, source->active);
        lua_setfield(lua, -2, "active_profile");
        lua_newtable(lua);
        for (size_t index = 0; index < source->profile_count; index++) {
            lua_pushstring(lua, source->profiles[index]);
            lua_rawseti(lua, -2, (int)index + 1);
        }
        lua_setfield(lua, -2, "profiles");
        if (lua_pcall(lua, 2, 0, 0) != 0) {
            log_lua_error(block, "power profile event");
            fault_block(block);
        }
    }
}

static int power_profiles_active_reply(sd_bus_message *message, void *userdata,
                                       sd_bus_error *ret_error) {
    (void)ret_error;
    struct power_profiles_source *source = userdata;
    source->active_query_inflight = false;
    const char *active;
    int result =
        sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "s");
    if (result >= 0) {
        result = sd_bus_message_read(message, "s", &active);
    }
    if (result < 0 || active == NULL ||
        strlen(active) > I3SD_POWER_PROFILE_NAME_LIMIT) {
        source->active_valid = false;
        return 0;
    }
    const bool changed =
        !source->active_valid || strcmp(source->active, active) != 0;
    strcpy(source->active, active);
    source->active_valid = true;
    if (changed) {
        notify_power_profiles_subscribers(source->app);
    }
    return 0;
}

static int power_profiles_list_reply(sd_bus_message *message, void *userdata,
                                     sd_bus_error *ret_error) {
    (void)ret_error;
    struct power_profiles_source *source = userdata;
    source->profiles_query_inflight = false;
    char parsed[I3SD_POWER_PROFILE_LIMIT][I3SD_POWER_PROFILE_NAME_LIMIT + 1] = {
        {0}};
    size_t parsed_count = 0;
    int result =
        sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "aa{sv}");
    if (result > 0) {
        result =
            sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "a{sv}");
    }
    while (result > 0 && (result = sd_bus_message_enter_container(
                              message, SD_BUS_TYPE_ARRAY, "{sv}")) > 0) {
        char profile[I3SD_POWER_PROFILE_NAME_LIMIT + 1] = {0};
        while ((result = sd_bus_message_enter_container(
                    message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
            const char *key;
            result = sd_bus_message_read(message, "s", &key);
            if (result < 0) {
                break;
            }
            if (strcmp(key, "Profile") == 0) {
                const char *value;
                result = sd_bus_message_enter_container(
                    message, SD_BUS_TYPE_VARIANT, "s");
                if (result > 0) {
                    result = sd_bus_message_read(message, "s", &value);
                }
                if (result >= 0 && value != NULL &&
                    strlen(value) <= I3SD_POWER_PROFILE_NAME_LIMIT) {
                    strcpy(profile, value);
                }
                if (result >= 0) {
                    result = sd_bus_message_exit_container(message);
                }
            } else {
                result = sd_bus_message_skip(message, "v");
            }
            if (result >= 0) {
                result = sd_bus_message_exit_container(message);
            }
            if (result < 0) {
                break;
            }
        }
        if (result >= 0) {
            result = sd_bus_message_exit_container(message);
        }
        if (profile[0] != '\0' && parsed_count < I3SD_POWER_PROFILE_LIMIT) {
            strcpy(parsed[parsed_count++], profile);
        }
    }
    if (result < 0 || parsed_count == 0) {
        source->profiles_valid = false;
        return 0;
    }
    bool changed =
        !source->profiles_valid || source->profile_count != parsed_count;
    if (!changed) {
        changed = memcmp(source->profiles, parsed,
                         parsed_count * sizeof(parsed[0])) != 0;
    }
    memcpy(source->profiles, parsed, parsed_count * sizeof(parsed[0]));
    source->profile_count = parsed_count;
    source->profiles_valid = true;
    if (changed) {
        notify_power_profiles_subscribers(source->app);
    }
    return 0;
}

static void
request_power_profiles_snapshots(struct power_profiles_source *source) {
    if (source->bus == NULL) {
        return;
    }
    if (!source->active_query_inflight) {
        int result = sd_bus_call_method_async(
            source->bus, NULL, "org.freedesktop.UPower.PowerProfiles",
            "/org/freedesktop/UPower/PowerProfiles",
            "org.freedesktop.DBus.Properties", "Get",
            power_profiles_active_reply, source, "ss",
            "org.freedesktop.UPower.PowerProfiles", "ActiveProfile");
        source->active_query_inflight = result >= 0;
    }
    if (!source->profiles_query_inflight) {
        int result = sd_bus_call_method_async(
            source->bus, NULL, "org.freedesktop.UPower.PowerProfiles",
            "/org/freedesktop/UPower/PowerProfiles",
            "org.freedesktop.DBus.Properties", "Get", power_profiles_list_reply,
            source, "ss", "org.freedesktop.UPower.PowerProfiles", "Profiles");
        source->profiles_query_inflight = result >= 0;
    }
}

static int power_profiles_changed(sd_bus_message *message, void *userdata,
                                  sd_bus_error *ret_error) {
    (void)message;
    (void)ret_error;
    request_power_profiles_snapshots(userdata);
    return 0;
}

static int power_profiles_match_installed(sd_bus_message *message,
                                          void *userdata,
                                          sd_bus_error *ret_error) {
    (void)ret_error;
    if (!sd_bus_message_is_method_error(message, NULL)) {
        request_power_profiles_snapshots(userdata);
    }
    return 0;
}

static void close_power_profiles_source(struct power_profiles_source *source) {
    if (source->registered_fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->registered_fd,
                  NULL);
    }
    source->registered_fd = -1;
    source->registered_events = 0;
    source->match_slot = sd_bus_slot_unref(source->match_slot);
    source->bus = sd_bus_flush_close_unref(source->bus);
    source->active_valid = false;
    source->profiles_valid = false;
    source->active_query_inflight = false;
    source->profiles_query_inflight = false;
}

static bool open_power_profiles_source(struct power_profiles_source *source,
                                       uint64_t now_ns) {
    int result = sd_bus_open_system(&source->bus);
    if (result < 0) {
        source->bus = NULL;
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    sd_bus_set_exit_on_disconnect(source->bus, 0);
    static const char match[] =
        "type='signal',sender='org.freedesktop.UPower.PowerProfiles',"
        "path='/org/freedesktop/UPower/PowerProfiles',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',"
        "arg0='org.freedesktop.UPower.PowerProfiles'";
    result = sd_bus_add_match_async(source->bus, &source->match_slot, match,
                                    power_profiles_changed,
                                    power_profiles_match_installed, source);
    if (result < 0) {
        close_power_profiles_source(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->retry_deadline_ns = 0;
    return true;
}

static void
reconcile_power_profiles_source(struct power_profiles_source *source,
                                uint64_t now_ns) {
    if (!power_profiles_needed(source->app)) {
        if (source->bus != NULL) {
            close_power_profiles_source(source);
        }
        return;
    }
    if (source->bus == NULL) {
        if (source->retry_deadline_ns > now_ns ||
            !open_power_profiles_source(source, now_ns)) {
            return;
        }
    }
    const int fd = sd_bus_get_fd(source->bus);
    const int poll_events = sd_bus_get_events(source->bus);
    if (fd < 0 || poll_events < 0) {
        close_power_profiles_source(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return;
    }
    uint32_t events = EPOLLERR | EPOLLHUP;
    events |= (poll_events & POLLIN) != 0 ? EPOLLIN : 0;
    events |= (poll_events & POLLOUT) != 0 ? EPOLLOUT : 0;
    if (source->registered_fd == fd && source->registered_events == events) {
        return;
    }
    if (source->registered_fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->registered_fd,
                  NULL);
    }
    struct epoll_event event = {
        .events = events,
        .data.u64 = SOURCE_POWER_PROFILES,
    };
    if (epoll_ctl(source->app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        close_power_profiles_source(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return;
    }
    source->registered_fd = fd;
    source->registered_events = events;
}

static void process_power_profiles_source(struct power_profiles_source *source,
                                          uint64_t now_ns) {
    if (source->bus == NULL) {
        return;
    }
    for (size_t count = 0; count < 256; count++) {
        int result = sd_bus_process(source->bus, NULL);
        if (result > 0) {
            continue;
        }
        if (result < 0) {
            close_power_profiles_source(source);
            source->retry_deadline_ns = now_ns + 5000000000ULL;
        }
        break;
    }
    reconcile_power_profiles_source(source, now_ns);
}

static int power_profile_set_reply(sd_bus_message *message, void *userdata,
                                   sd_bus_error *ret_error) {
    (void)userdata;
    (void)ret_error;
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *error = sd_bus_message_get_error(message);
        fprintf(stderr, "i3sd: setting power profile failed: %s\n",
                error != NULL && error->message != NULL ? error->message
                                                        : "D-Bus error");
    }
    return 0;
}

static bool set_power_profile(struct app *app, const char *profile) {
    struct power_profiles_source *source = &app->power_profiles;
    if (source->bus == NULL) {
        return false;
    }
    return sd_bus_call_method_async(source->bus, NULL,
                                    "org.freedesktop.UPower.PowerProfiles",
                                    "/org/freedesktop/UPower/PowerProfiles",
                                    "org.freedesktop.DBus.Properties", "Set",
                                    power_profile_set_reply, source, "ssv",
                                    "org.freedesktop.UPower.PowerProfiles",
                                    "ActiveProfile", "s", profile) >= 0;
}

static bool known_power_profile(const struct power_profiles_source *source,
                                const char *profile) {
    for (size_t index = 0; index < source->profile_count; index++) {
        if (strcmp(source->profiles[index], profile) == 0) {
            return true;
        }
    }
    return false;
}

static bool write_rofi_input(int fd, const char *input, size_t input_len) {
    size_t offset = 0;
    while (offset < input_len) {
        const ssize_t count = write(fd, input + offset, input_len - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool open_rofi_menu(struct app *app, struct block *block,
                           const char *prompt, const char *choices,
                           size_t choices_len, int callback_ref) {
    if (app->rofi.output_fd >= 0) {
        return false;
    }
    /* A private input fd avoids blocking the reactor on a pipe writer and
       prevents a short-lived rofi process from generating SIGPIPE. */
    const int input_fd = memfd_create("i3sd-rofi-input", MFD_CLOEXEC);
    if (input_fd < 0 || !write_rofi_input(input_fd, choices, choices_len) ||
        lseek(input_fd, 0, SEEK_SET) < 0) {
        if (input_fd >= 0) {
            close(input_fd);
        }
        return false;
    }
    int output_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) < 0) {
        close(input_fd);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }
    if (pid == 0) {
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        sigprocmask(SIG_SETMASK, &empty_mask, NULL);
        if (dup2(input_fd, STDIN_FILENO) < 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(input_fd);
        close(output_pipe[0]);
        close(output_pipe[1]);
        execlp("rofi", "rofi", "-dmenu", "-p", prompt, NULL);
        _exit(127);
    }
    close(input_fd);
    close(output_pipe[1]);
    if (!make_nonblocking(output_pipe[0])) {
        close(output_pipe[0]);
        kill(pid, SIGTERM);
        return false;
    }
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLERR | EPOLLHUP,
        .data.u64 = SOURCE_ROFI,
    };
    if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, output_pipe[0], &event) < 0) {
        close(output_pipe[0]);
        kill(pid, SIGTERM);
        return false;
    }
    app->rofi.pid = pid;
    app->rofi.output_fd = output_pipe[0];
    app->rofi.block = block;
    app->rofi.callback_ref = callback_ref;
    app->rofi.result_len = 0;
    return true;
}

static void cancel_rofi_menu(struct app *app,
                             const struct generation *generation) {
    struct block *block = app->rofi.block;
    if (block == NULL || block->generation != generation) {
        return;
    }
    if (app->rofi.output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, app->rofi.output_fd, NULL);
        close(app->rofi.output_fd);
        app->rofi.output_fd = -1;
    }
    if (app->rofi.pid > 0) {
        kill(app->rofi.pid, SIGTERM);
        app->rofi.pid = 0;
    }
    luaL_unref(generation->lua, LUA_REGISTRYINDEX, app->rofi.callback_ref);
    app->rofi.block = NULL;
    app->rofi.callback_ref = LUA_NOREF;
    app->rofi.result_len = 0;
}

static void finish_rofi_menu(struct app *app) {
    if (app->rofi.output_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, app->rofi.output_fd, NULL);
        close(app->rofi.output_fd);
        app->rofi.output_fd = -1;
    }
    struct block *block = app->rofi.block;
    const int callback_ref = app->rofi.callback_ref;
    app->rofi.block = NULL;
    app->rofi.callback_ref = LUA_NOREF;

    while (app->rofi.result_len > 0 &&
           (app->rofi.result[app->rofi.result_len - 1] == '\n' ||
            app->rofi.result[app->rofi.result_len - 1] == '\r')) {
        app->rofi.result_len--;
    }
    const bool valid_selection =
        app->rofi.result_len > 0 &&
        app->rofi.result_len <= I3SD_ROFI_CHOICE_BYTES &&
        valid_text(app->rofi.result, app->rofi.result_len,
                   I3SD_ROFI_CHOICE_BYTES) &&
        memchr(app->rofi.result, '\n', app->rofi.result_len) == NULL &&
        memchr(app->rofi.result, '\r', app->rofi.result_len) == NULL;

    if (block != NULL) {
        lua_State *lua = block->generation->lua;
        if (app->current == block->generation && !block->faulted) {
            lua_rawgeti(lua, LUA_REGISTRYINDEX, callback_ref);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
            if (valid_selection) {
                lua_pushlstring(lua, app->rofi.result, app->rofi.result_len);
            } else {
                lua_pushnil(lua);
            }
            if (lua_pcall(lua, 2, 0, 0) != 0) {
                log_lua_error(block, "rofi callback");
                fault_block(block);
            }
        }
        luaL_unref(lua, LUA_REGISTRYINDEX, callback_ref);
    }
    app->rofi.result_len = 0;
}

static void read_rofi_menu(struct app *app) {
    while (app->rofi.result_len < I3SD_ROFI_CHOICE_BYTES + 1) {
        ssize_t count =
            read(app->rofi.output_fd, app->rofi.result + app->rofi.result_len,
                 I3SD_ROFI_CHOICE_BYTES + 1 - app->rofi.result_len);
        if (count > 0) {
            app->rofi.result_len += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        finish_rofi_menu(app);
        return;
    }
    finish_rofi_menu(app);
}

static void generation_destroy(struct generation *generation) {
    if (generation == NULL) {
        return;
    }
    /* A popup callback is owned by the generation's Lua registry. */
    cancel_rofi_menu(generation->app, generation);
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
    /* Lua finalizers can still inspect native handles, so close Lua first. */
    if (generation->lua != NULL) {
        lua_close(generation->lua);
    }
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

static void fault_block(struct block *block) {
    if (block->faulted) {
        return;
    }
    block->faulted = true;
    struct generation *generation = block->generation;
    if (generation->app->rofi.block == block) {
        cancel_rofi_menu(generation->app, generation);
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
            fault_block(timer->block);
        }
    }
    for (size_t index = 0; index < candidate->block_count; index++) {
        struct block *block = candidate->blocks[index];
        if (!call_block_ref(block, block->update_ref, "live refresh", 0)) {
            fault_block(block);
        } else if (!add_poll_timer(block, activation_ns)) {
            fprintf(stderr, "i3sd: unable to activate polling timer for %s\n",
                    block->name);
            fault_block(block);
        }
    }
    generation_destroy(old);
    /* Reuse an authoritative shared-bus snapshot across generation reloads. */
    notify_systemd_subscribers(app);
    notify_power_profiles_subscribers(app);
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
        log_lua_error(target, "click");
        fault_block(target);
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
                        fault_block(block);
                    }
                }
                break;
            case SIGCHLD: {
                pid_t child;
                do {
                    child = waitpid(-1, NULL, WNOHANG);
                    if (child == app->rofi.pid) {
                        app->rofi.pid = 0;
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
            fault_block(timer->block);
            continue;
        }
        if (native->interval_ns != 0 && !timer->cancelled &&
            !timer->block->faulted &&
            !i3sd_timer_reschedule_fixed(&app->timer_heap, native, now_ns)) {
            fprintf(stderr, "i3sd: repeating timer overflow\n");
            fault_block(timer->block);
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
        if (!systemd_bus_needed(app, source->scope)) {
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
    if (power_profiles_needed(app)) {
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
    app->rofi.output_fd = -1;
    app->rofi.callback_ref = LUA_NOREF;
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
        reconcile_systemd_bus(&app->systemd_buses[0], now_ns);
        reconcile_systemd_bus(&app->systemd_buses[1], now_ns);
        reconcile_power_profiles_source(&app->power_profiles, now_ns);
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
                process_systemd_bus(&app->systemd_buses[0], monotonic_now_ns());
                break;
            case SOURCE_USER_BUS:
                process_systemd_bus(&app->systemd_buses[1], monotonic_now_ns());
                break;
            case SOURCE_POWER_PROFILES:
                process_power_profiles_source(&app->power_profiles,
                                              monotonic_now_ns());
                break;
            case SOURCE_ROFI:
                read_rofi_menu(app);
                break;
            default:
                break;
            }
        }

        now_ns = monotonic_now_ns();
        process_systemd_bus(&app->systemd_buses[0], now_ns);
        process_systemd_bus(&app->systemd_buses[1], now_ns);
        process_power_profiles_source(&app->power_profiles, now_ns);
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
    generation_destroy(app->current);
    if (app->systemd_buses[0].bus != NULL) {
        close_systemd_bus(&app->systemd_buses[0]);
    }
    if (app->systemd_buses[1].bus != NULL) {
        close_systemd_bus(&app->systemd_buses[1]);
    }
    if (app->power_profiles.bus != NULL) {
        close_power_profiles_source(&app->power_profiles);
    }
    if (app->rofi.output_fd >= 0) {
        close(app->rofi.output_fd);
    }
    if (app->rofi.pid > 0) {
        kill(app->rofi.pid, SIGTERM);
        waitpid(app->rofi.pid, NULL, 0);
    }
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
