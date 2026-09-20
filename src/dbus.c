#define _GNU_SOURCE

#include "dbus.h"
#include "i3sd/buffer.h"
#include "i3sd/utf8.h"

#include <lauxlib.h>
#include <systemd/sd-bus.h>

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#define I3SD_DBUS_OPERATION_LIMIT 1024U
#define I3SD_DBUS_PROCESS_BUDGET 256U
#define I3SD_DBUS_MATCH_LIMIT 8192U
#define I3SD_DBUS_MESSAGE_LIMIT (1024U * 1024U)
#define I3SD_DBUS_VALUE_LIMIT 65536U
#define I3SD_DBUS_CONTAINER_DEPTH 32U

enum dbus_scope {
    DBUS_SCOPE_SYSTEM,
    DBUS_SCOPE_USER,
};

struct i3sd_dbus_runtime;

struct dbus_transport {
    struct i3sd_dbus_runtime *app;
    sd_bus *bus;
    uint64_t cookie;
    uint64_t epoch;
    uint64_t retry_deadline_ns;
    uint32_t failed_count;
    int registered_fd;
    uint32_t registered_events;
    enum dbus_scope scope;
};

struct i3sd_dbus_generation;

struct dbus_owner {
    struct i3sd_dbus_generation *generation;
    struct dbus_owner *next;
    void *userdata;
    bool faulted;
};

struct dbus_match {
    struct dbus_match *next;
    struct dbus_owner *block;
    sd_bus_slot *slot;
    char *rule;
    int callback_ref;
    int install_ref;
    uint64_t attempted_epoch;
    enum dbus_scope scope;
    bool cancelled;
};

struct dbus_call {
    struct dbus_call *next;
    struct dbus_owner *block;
    sd_bus_slot *slot;
    char *destination;
    char *path;
    char *interface;
    char *member;
    char *signature;
    int args_ref;
    int callback_ref;
    uint64_t timeout_us;
    uint64_t id;
    enum dbus_scope scope;
    bool sent;
    bool completed;
    bool cancelled;
};

struct dbus_connect_watch {
    struct dbus_connect_watch *next;
    struct dbus_owner *block;
    int callback_ref;
    uint64_t delivered_epoch;
    enum dbus_scope scope;
    bool cancelled;
};

struct i3sd_dbus_generation {
    lua_State *lua;
    struct i3sd_dbus_host host;
    struct dbus_owner *owners;
    struct dbus_match *dbus_matches;
    struct dbus_call *dbus_calls;
    struct dbus_connect_watch *dbus_connect_watches;
    size_t dbus_operation_count;
    uint64_t next_dbus_call_id;
};

struct i3sd_dbus_runtime {
    int epoll_fd;
    uint64_t *next_registration_cookie;
    struct i3sd_dbus_generation *current;
    struct dbus_transport transports[2];
};

struct lua_dbus_bus {
    struct dbus_owner *block;
    enum dbus_scope scope;
};

enum lua_dbus_handle_kind {
    LUA_DBUS_MATCH,
    LUA_DBUS_CALL,
    LUA_DBUS_CONNECT_WATCH,
};

struct lua_dbus_handle {
    enum lua_dbus_handle_kind kind;
    union {
        struct dbus_match *match;
        struct {
            struct i3sd_dbus_generation *generation;
            uint64_t id;
        } call;
        struct dbus_connect_watch *connect_watch;
    } value;
};

struct lua_dbus_message {
    sd_bus_message *message;
    char consumed[256];
    size_t consumed_len;
    bool valid;
};

struct lua_dbus_variant {
    char *signature;
    int value_ref;
};

static const char dbus_bus_metatable[] = "i3sd.dbus_bus";
static const char dbus_handle_metatable[] = "i3sd.dbus_handle";
static const char dbus_message_metatable[] = "i3sd.dbus_message";
static const char dbus_variant_metatable[] = "i3sd.dbus_variant";

static int absolute_lua_index(lua_State *lua, int index) {
    if (index > 0 || index <= LUA_REGISTRYINDEX) {
        return index;
    }
    return lua_gettop(lua) + index + 1;
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

static char *copy_bytes(const char *value, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
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

static void dbus_push_uint64(lua_State *lua, uint64_t value) {
    lua_getfield(lua, LUA_REGISTRYINDEX, "i3sd.dbus.generation");
    struct i3sd_dbus_generation *generation = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    generation->host.push_uint64(lua, value);
}

static void dbus_push_int64(lua_State *lua, int64_t value) {
    lua_getfield(lua, LUA_REGISTRYINDEX, "i3sd.dbus.generation");
    struct i3sd_dbus_generation *generation = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    generation->host.push_int64(lua, value);
}

static void log_dbus_error(struct dbus_owner *owner, const char *phase) {
    owner->generation->host.log_lua_error(owner->userdata, phase);
}

static void fault_dbus_owner(struct dbus_owner *owner);

struct dbus_signature {
    const char *cursor;
    const char *end;
};

static bool dbus_basic_type(char type) {
    return strchr("ybnqiuxtdsog", type) != NULL;
}

static bool dbus_signature_type(struct dbus_signature *signature,
                                unsigned depth, bool allow_dict) {
    /* Parse exactly one complete type while enforcing D-Bus container rules. */
    if (depth > I3SD_DBUS_CONTAINER_DEPTH ||
        signature->cursor == signature->end) {
        return false;
    }
    const char type = *signature->cursor++;
    if (dbus_basic_type(type) || type == 'v') {
        return true;
    }
    if (type == 'a') {
        return dbus_signature_type(signature, depth + 1, true);
    }
    if (type == '(') {
        const char *first = signature->cursor;
        while (signature->cursor < signature->end &&
               *signature->cursor != ')') {
            if (!dbus_signature_type(signature, depth + 1, false)) {
                return false;
            }
        }
        return signature->cursor > first &&
               signature->cursor < signature->end &&
               *signature->cursor++ == ')';
    }
    if (type == '{' && allow_dict) {
        if (signature->cursor == signature->end ||
            !dbus_basic_type(*signature->cursor) ||
            !dbus_signature_type(signature, depth + 1, false) ||
            !dbus_signature_type(signature, depth + 1, false) ||
            signature->cursor == signature->end ||
            *signature->cursor++ != '}') {
            return false;
        }
        return true;
    }
    return false;
}

static bool dbus_valid_signature(const char *signature, bool single) {
    const size_t len = strlen(signature);
    if (len > 255) {
        return false;
    }
    struct dbus_signature parser = {
        .cursor = signature,
        .end = signature + len,
    };
    size_t count = 0;
    while (parser.cursor < parser.end) {
        if (!dbus_signature_type(&parser, 0, false)) {
            return false;
        }
        count++;
    }
    return (!single || count == 1) && (!single || len > 0);
}

static const char *dbus_one_type_end(const char *signature) {
    struct dbus_signature parser = {
        .cursor = signature,
        .end = signature + strlen(signature),
    };
    if (!dbus_signature_type(&parser, 0, false)) {
        return NULL;
    }
    return parser.cursor;
}

static bool lua_sequence_is_dense(lua_State *lua, int index, size_t length) {
    index = absolute_lua_index(lua, index);
    size_t seen = 0;
    lua_pushnil(lua);
    while (lua_next(lua, index) != 0) {
        if (lua_type(lua, -2) != LUA_TNUMBER) {
            lua_pop(lua, 2);
            return false;
        }
        const lua_Number key = lua_tonumber(lua, -2);
        if (!isfinite(key) || floor(key) != key || key < 1 ||
            key > (lua_Number)length) {
            lua_pop(lua, 2);
            return false;
        }
        seen++;
        lua_pop(lua, 1);
    }
    return seen == length;
}

static bool lua_exact_integer(lua_State *lua, int index, bool is_unsigned,
                              uint64_t *bits) {
    if (lua_type(lua, index) == LUA_TNUMBER) {
        const lua_Number number = lua_tonumber(lua, index);
        if (!isfinite(number) || floor(number) != number ||
            fabs(number) > 9007199254740991.0 || (is_unsigned && number < 0)) {
            return false;
        }
        *bits = is_unsigned ? (uint64_t)number : (uint64_t)(int64_t)number;
        return true;
    }

    /* LuaJIT does not expose cdata through its public C API. Its canonical
     * integer tostring form is exact and includes an LL/ULL suffix. */
    lua_getglobal(lua, "tostring");
    lua_pushvalue(lua, index);
    if (lua_pcall(lua, 1, 1, 0) != 0) {
        lua_pop(lua, 1);
        return false;
    }
    const char *text = lua_tostring(lua, -1);
    if (text == NULL) {
        lua_pop(lua, 1);
        return false;
    }
    errno = 0;
    char *end;
    if (is_unsigned) {
        unsigned long long value = strtoull(text, &end, 10);
        const bool valid = errno == 0 && end != text && strcmp(end, "ULL") == 0;
        if (valid) {
            *bits = (uint64_t)value;
        }
        lua_pop(lua, 1);
        return valid;
    }
    long long value = strtoll(text, &end, 10);
    const bool valid = errno == 0 && end != text && strcmp(end, "LL") == 0;
    if (valid) {
        *bits = (uint64_t)(int64_t)value;
    }
    lua_pop(lua, 1);
    return valid;
}

static bool validate_dbus_value(lua_State *lua, const char *start,
                                const char *end, int index, unsigned depth,
                                size_t *value_count, const char **error);

static bool validate_dbus_sequence(lua_State *lua, const char *start,
                                   const char *end, int table, unsigned depth,
                                   size_t *value_count, const char **error) {
    table = absolute_lua_index(lua, table);
    if (lua_type(lua, table) != LUA_TTABLE) {
        *error = "D-Bus container value must be a table";
        return false;
    }
    size_t item = 1;
    const char *cursor = start;
    while (cursor < end) {
        const char *next = dbus_one_type_end(cursor);
        if (next == NULL || next > end) {
            *error = "invalid D-Bus signature";
            return false;
        }
        lua_rawgeti(lua, table, (int)item);
        const bool valid = !lua_isnil(lua, -1) &&
                           validate_dbus_value(lua, cursor, next, -1, depth,
                                               value_count, error);
        lua_pop(lua, 1);
        if (!valid) {
            if (*error == NULL) {
                *error = "D-Bus argument count does not match signature";
            }
            return false;
        }
        item++;
        cursor = next;
    }
    const size_t length = lua_objlen(lua, table);
    if (length != item - 1 || !lua_sequence_is_dense(lua, table, length)) {
        *error = "D-Bus values must be a dense 1-based sequence";
        return false;
    }
    return true;
}

static bool validate_dbus_value(lua_State *lua, const char *start,
                                const char *end, int index, unsigned depth,
                                size_t *value_count, const char **error) {
    index = absolute_lua_index(lua, index);
    if (depth > I3SD_DBUS_CONTAINER_DEPTH ||
        ++*value_count > I3SD_DBUS_VALUE_LIMIT) {
        *error = "D-Bus value exceeds the container or value limit";
        return false;
    }
    const char type = *start;
    if (type == 'y' || type == 'n' || type == 'q' || type == 'i' ||
        type == 'u') {
        if (lua_type(lua, index) != LUA_TNUMBER) {
            *error = "D-Bus integer value must be a number";
            return false;
        }
        const lua_Number value = lua_tonumber(lua, index);
        lua_Number minimum = 0;
        lua_Number maximum = 0;
        switch (type) {
        case 'y':
            maximum = UINT8_MAX;
            break;
        case 'n':
            minimum = INT16_MIN;
            maximum = INT16_MAX;
            break;
        case 'q':
            maximum = UINT16_MAX;
            break;
        case 'i':
            minimum = INT32_MIN;
            maximum = INT32_MAX;
            break;
        default:
            maximum = UINT32_MAX;
            break;
        }
        if (!isfinite(value) || floor(value) != value || value < minimum ||
            value > maximum) {
            *error = "D-Bus integer value is out of range";
            return false;
        }
        return true;
    }
    if (type == 'x' || type == 't') {
        uint64_t ignored;
        if (!lua_exact_integer(lua, index, type == 't', &ignored)) {
            *error =
                "D-Bus 64-bit integer must be an exact number or LuaJIT cdata";
            return false;
        }
        return true;
    }
    if (type == 'b') {
        if (lua_type(lua, index) != LUA_TBOOLEAN) {
            *error = "D-Bus boolean value must be a boolean";
            return false;
        }
        return true;
    }
    if (type == 'd') {
        if (lua_type(lua, index) != LUA_TNUMBER) {
            *error = "D-Bus double value must be a number";
            return false;
        }
        return true;
    }
    if (type == 's' || type == 'o' || type == 'g') {
        size_t length;
        const char *value = lua_tolstring(lua, index, &length);
        if (value == NULL || length > I3SD_DBUS_MESSAGE_LIMIT ||
            strlen(value) != length ||
            (type == 's' && !i3sd_valid_utf8(value, length)) ||
            (type == 'o' && !sd_bus_object_path_is_valid(value)) ||
            (type == 'g' && !dbus_valid_signature(value, false))) {
            *error = "invalid D-Bus string, object path, or signature value";
            return false;
        }
        return true;
    }
    if (type == 'v') {
        struct lua_dbus_variant *variant =
            luaL_testudata(lua, index, dbus_variant_metatable);
        if (variant == NULL || variant->signature == NULL) {
            *error = "D-Bus variant must be created with i3sd.dbus.variant";
            return false;
        }
        lua_rawgeti(lua, LUA_REGISTRYINDEX, variant->value_ref);
        const char *variant_end =
            variant->signature + strlen(variant->signature);
        const bool valid =
            validate_dbus_value(lua, variant->signature, variant_end, -1,
                                depth + 1, value_count, error);
        lua_pop(lua, 1);
        return valid;
    }
    if (type == 'a') {
        const char *element = start + 1;
        if (*element == 'y' && element + 1 == end &&
            lua_type(lua, index) == LUA_TSTRING) {
            size_t length;
            lua_tolstring(lua, index, &length);
            if (length > I3SD_DBUS_MESSAGE_LIMIT) {
                *error = "D-Bus byte array exceeds the message limit";
                return false;
            }
            return true;
        }
        if (lua_type(lua, index) != LUA_TTABLE) {
            *error = "D-Bus array value must be a table";
            return false;
        }
        const size_t length = lua_objlen(lua, index);
        if (!lua_sequence_is_dense(lua, index, length)) {
            *error = "D-Bus array must be a dense 1-based sequence";
            return false;
        }
        for (size_t item = 1; item <= length; item++) {
            lua_rawgeti(lua, index, (int)item);
            const bool valid = validate_dbus_value(
                lua, element, end, -1, depth + 1, value_count, error);
            lua_pop(lua, 1);
            if (!valid) {
                return false;
            }
        }
        return true;
    }
    if (type == '(' || type == '{') {
        return validate_dbus_sequence(lua, start + 1, end - 1, index, depth + 1,
                                      value_count, error);
    }
    *error = "unsupported D-Bus type";
    return false;
}

static int encode_dbus_value(sd_bus_message *message, lua_State *lua,
                             const char *start, const char *end, int index);

static int encode_dbus_sequence(sd_bus_message *message, lua_State *lua,
                                const char *start, const char *end, int table) {
    table = absolute_lua_index(lua, table);
    size_t item = 1;
    for (const char *cursor = start; cursor < end; item++) {
        const char *next = dbus_one_type_end(cursor);
        lua_rawgeti(lua, table, (int)item);
        int result = encode_dbus_value(message, lua, cursor, next, -1);
        lua_pop(lua, 1);
        if (result < 0) {
            return result;
        }
        cursor = next;
    }
    return 0;
}

static int encode_dbus_value(sd_bus_message *message, lua_State *lua,
                             const char *start, const char *end, int index) {
    index = absolute_lua_index(lua, index);
    const char type = *start;
    if (type == 'y') {
        uint8_t value = (uint8_t)lua_tointeger(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'b') {
        int value = lua_toboolean(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'n') {
        int16_t value = (int16_t)lua_tointeger(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'q') {
        uint16_t value = (uint16_t)lua_tointeger(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'i') {
        int32_t value = (int32_t)lua_tointeger(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'u') {
        uint32_t value = (uint32_t)lua_tonumber(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'x' || type == 't') {
        uint64_t value;
        if (!lua_exact_integer(lua, index, type == 't', &value)) {
            return -EINVAL;
        }
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 'd') {
        double value = lua_tonumber(lua, index);
        return sd_bus_message_append_basic(message, type, &value);
    }
    if (type == 's' || type == 'o' || type == 'g') {
        const char *value = lua_tostring(lua, index);
        return sd_bus_message_append_basic(message, type, value);
    }
    if (type == 'v') {
        struct lua_dbus_variant *variant =
            luaL_checkudata(lua, index, dbus_variant_metatable);
        int result =
            sd_bus_message_open_container(message, 'v', variant->signature);
        if (result < 0) {
            return result;
        }
        lua_rawgeti(lua, LUA_REGISTRYINDEX, variant->value_ref);
        result = encode_dbus_value(
            message, lua, variant->signature,
            variant->signature + strlen(variant->signature), -1);
        lua_pop(lua, 1);
        return result < 0 ? result : sd_bus_message_close_container(message);
    }

    char contents[256];
    const char *content_start = start + 1;
    const char *content_end = type == 'a' ? end : end - 1;
    const size_t content_len = (size_t)(content_end - content_start);
    memcpy(contents, content_start, content_len);
    contents[content_len] = '\0';
    if (type == 'a' && *content_start == 'y' && content_start + 1 == end &&
        lua_type(lua, index) == LUA_TSTRING) {
        size_t length;
        const void *bytes = lua_tolstring(lua, index, &length);
        return sd_bus_message_append_array(message, 'y', bytes, length);
    }
    const char container_type = type == 'a' ? 'a' : type == '(' ? 'r' : 'e';
    int result =
        sd_bus_message_open_container(message, container_type, contents);
    if (result < 0) {
        return result;
    }
    if (type == 'a') {
        const size_t length = lua_objlen(lua, index);
        for (size_t item = 1; item <= length; item++) {
            lua_rawgeti(lua, index, (int)item);
            result =
                encode_dbus_value(message, lua, content_start, content_end, -1);
            lua_pop(lua, 1);
            if (result < 0) {
                return result;
            }
        }
    } else {
        result = encode_dbus_sequence(message, lua, content_start, content_end,
                                      index);
        if (result < 0) {
            return result;
        }
    }
    return sd_bus_message_close_container(message);
}

static bool decode_dbus_value(sd_bus_message *message, lua_State *lua,
                              const char *start, const char *end,
                              unsigned depth, size_t *value_count);

static bool decode_dbus_sequence(sd_bus_message *message, lua_State *lua,
                                 const char *start, const char *end,
                                 unsigned depth, size_t *value_count) {
    lua_newtable(lua);
    int item = 1;
    for (const char *cursor = start; cursor < end; item++) {
        const char *next = dbus_one_type_end(cursor);
        if (!decode_dbus_value(message, lua, cursor, next, depth,
                               value_count)) {
            lua_pop(lua, 1);
            return false;
        }
        lua_rawseti(lua, -2, item);
        cursor = next;
    }
    return true;
}

static struct lua_dbus_bus *check_dbus_bus(lua_State *lua, int index) {
    return luaL_checkudata(lua, index, dbus_bus_metatable);
}

static char *required_dbus_string(lua_State *lua, int table,
                                  const char *field) {
    lua_getfield(lua, table, field);
    size_t length;
    const char *value = luaL_checklstring(lua, -1, &length);
    if (length == 0 || length > I3SD_DBUS_MATCH_LIMIT ||
        strlen(value) != length) {
        luaL_error(lua, "%s must be a nonempty NUL-free string", field);
    }
    char *copy = copy_bytes(value, length);
    lua_pop(lua, 1);
    if (copy == NULL) {
        luaL_error(lua, "out of memory copying %s", field);
    }
    return copy;
}

static bool dbus_match_field_name(const char *field) {
    static const char *const exact[] = {
        "sender",    "path",   "path_namespace",
        "interface", "member", "destination",
    };
    if (strict_known_key(field, exact, sizeof(exact) / sizeof(exact[0]))) {
        return true;
    }
    if (strncmp(field, "arg", 3) != 0) {
        return false;
    }
    const char *cursor = field + 3;
    char *end;
    errno = 0;
    unsigned long argument = strtoul(cursor, &end, 10);
    if (errno != 0 || end == cursor || argument > 63) {
        return false;
    }
    return *end == '\0' || strcmp(end, "path") == 0 ||
           strcmp(end, "namespace") == 0;
}

static bool append_dbus_match_bytes(struct i3sd_buffer *rule, const char *bytes,
                                    size_t length) {
    return i3sd_buffer_append(rule, bytes, length, I3SD_DBUS_MATCH_LIMIT);
}

static bool append_dbus_match_clause(struct i3sd_buffer *rule,
                                     const char *field, const char *value,
                                     size_t length) {
    if (!append_dbus_match_bytes(rule, ",", 1) ||
        !append_dbus_match_bytes(rule, field, strlen(field)) ||
        !append_dbus_match_bytes(rule, "='", 2)) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if ((value[index] == '\\' || value[index] == '\'') &&
            !append_dbus_match_bytes(rule, "\\", 1)) {
            return false;
        }
        if (!append_dbus_match_bytes(rule, &value[index], 1)) {
            return false;
        }
    }
    return append_dbus_match_bytes(rule, "'", 1);
}

static char *build_dbus_match_rule(lua_State *lua, int table) {
    table = absolute_lua_index(lua, table);
    struct i3sd_buffer rule = {0};
    if (!append_dbus_match_bytes(&rule, "type='signal'", 13)) {
        return NULL;
    }
    lua_pushnil(lua);
    while (lua_next(lua, table) != 0) {
        if (lua_type(lua, -2) != LUA_TSTRING) {
            i3sd_buffer_destroy(&rule);
            luaL_error(lua, "D-Bus match fields must have string keys");
        }
        const char *field = lua_tostring(lua, -2);
        if (!dbus_match_field_name(field)) {
            i3sd_buffer_destroy(&rule);
            luaL_error(lua, "unknown D-Bus match field '%s'", field);
        }
        size_t length;
        const char *value = luaL_checklstring(lua, -1, &length);
        const bool named_valid = ((strcmp(field, "sender") != 0 &&
                                   strcmp(field, "destination") != 0) ||
                                  sd_bus_service_name_is_valid(value)) &&
                                 ((strcmp(field, "path") != 0 &&
                                   strcmp(field, "path_namespace") != 0) ||
                                  sd_bus_object_path_is_valid(value)) &&
                                 (strcmp(field, "interface") != 0 ||
                                  sd_bus_interface_name_is_valid(value)) &&
                                 (strcmp(field, "member") != 0 ||
                                  sd_bus_member_name_is_valid(value));
        if (strlen(value) != length || !named_valid ||
            !append_dbus_match_clause(&rule, field, value, length)) {
            i3sd_buffer_destroy(&rule);
            luaL_error(lua, "invalid or oversized D-Bus match rule");
        }
        lua_pop(lua, 1);
    }
    if (!i3sd_buffer_append_char(&rule, '\0', I3SD_DBUS_MATCH_LIMIT)) {
        i3sd_buffer_destroy(&rule);
        return NULL;
    }
    return rule.data;
}

static struct lua_dbus_handle *new_dbus_handle(lua_State *lua,
                                               enum lua_dbus_handle_kind kind) {
    struct lua_dbus_handle *handle = lua_newuserdata(lua, sizeof(*handle));
    memset(handle, 0, sizeof(*handle));
    handle->kind = kind;
    luaL_getmetatable(lua, dbus_handle_metatable);
    lua_setmetatable(lua, -2);
    return handle;
}

static int lua_dbus_handle_cancel(lua_State *lua) {
    struct lua_dbus_handle *handle =
        luaL_checkudata(lua, 1, dbus_handle_metatable);
    if (handle->kind == LUA_DBUS_MATCH && handle->value.match != NULL) {
        handle->value.match->cancelled = true;
        handle->value.match->slot =
            sd_bus_slot_unref(handle->value.match->slot);
        handle->value.match = NULL;
    } else if (handle->kind == LUA_DBUS_CALL &&
               handle->value.call.generation != NULL) {
        struct i3sd_dbus_generation *generation = handle->value.call.generation;
        for (struct dbus_call *call = generation->dbus_calls; call != NULL;
             call = call->next) {
            if (call->id == handle->value.call.id) {
                call->cancelled = true;
                call->slot = sd_bus_slot_unref(call->slot);
                break;
            }
        }
        handle->value.call.generation = NULL;
    } else if (handle->kind == LUA_DBUS_CONNECT_WATCH &&
               handle->value.connect_watch != NULL) {
        handle->value.connect_watch->cancelled = true;
        handle->value.connect_watch = NULL;
    }
    return 0;
}

static int lua_dbus_match(lua_State *lua) {
    struct lua_dbus_bus *bus = check_dbus_bus(lua, 1);
    struct i3sd_dbus_generation *generation = bus->block->generation;
    luaL_checktype(lua, 2, LUA_TTABLE);
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    if (!lua_isnoneornil(lua, 4)) {
        luaL_checktype(lua, 4, LUA_TFUNCTION);
    }
    if (generation->dbus_operation_count == I3SD_DBUS_OPERATION_LIMIT) {
        return luaL_error(lua, "D-Bus operation limit reached");
    }
    char *rule = build_dbus_match_rule(lua, 2);
    if (rule == NULL) {
        return luaL_error(lua, "out of memory creating D-Bus match");
    }
    struct dbus_match *match = calloc(1, sizeof(*match));
    if (match == NULL) {
        free(rule);
        return luaL_error(lua, "out of memory creating D-Bus match");
    }
    match->block = bus->block;
    match->scope = bus->scope;
    match->rule = rule;
    lua_pushvalue(lua, 3);
    match->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    match->install_ref = LUA_NOREF;
    if (!lua_isnoneornil(lua, 4)) {
        lua_pushvalue(lua, 4);
        match->install_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    }
    match->next = generation->dbus_matches;
    generation->dbus_matches = match;
    generation->dbus_operation_count++;

    struct lua_dbus_handle *handle = new_dbus_handle(lua, LUA_DBUS_MATCH);
    handle->value.match = match;
    return 1;
}

static bool valid_dbus_call_names(const struct dbus_call *call) {
    return sd_bus_service_name_is_valid(call->destination) &&
           sd_bus_object_path_is_valid(call->path) &&
           sd_bus_interface_name_is_valid(call->interface) &&
           sd_bus_member_name_is_valid(call->member);
}

static int lua_dbus_call(lua_State *lua) {
    struct lua_dbus_bus *bus = check_dbus_bus(lua, 1);
    struct i3sd_dbus_generation *generation = bus->block->generation;
    luaL_checktype(lua, 2, LUA_TTABLE);
    luaL_checktype(lua, 3, LUA_TFUNCTION);
    static const char *const fields[] = {
        "destination", "path", "interface", "member",
        "signature",   "args", "timeout",
    };
    check_strict_table(lua, 2, fields, sizeof(fields) / sizeof(fields[0]));
    if (generation->dbus_operation_count == I3SD_DBUS_OPERATION_LIMIT) {
        return luaL_error(lua, "D-Bus operation limit reached");
    }

    struct dbus_call *call = calloc(1, sizeof(*call));
    if (call == NULL) {
        return luaL_error(lua, "out of memory creating D-Bus call");
    }
    call->args_ref = call->callback_ref = LUA_NOREF;
    call->block = bus->block;
    call->scope = bus->scope;
    generation->next_dbus_call_id++;
    if (generation->next_dbus_call_id == 0) {
        free(call);
        return luaL_error(lua, "D-Bus call identifier exhausted");
    }
    call->id = generation->next_dbus_call_id;
    call->destination = required_dbus_string(lua, 2, "destination");
    call->path = required_dbus_string(lua, 2, "path");
    call->interface = required_dbus_string(lua, 2, "interface");
    call->member = required_dbus_string(lua, 2, "member");
    lua_getfield(lua, 2, "signature");
    call->signature =
        strdup(lua_isnil(lua, -1) ? "" : luaL_checkstring(lua, -1));
    lua_pop(lua, 1);
    if (call->signature == NULL) {
        return luaL_error(lua, "out of memory copying D-Bus signature");
    }
    if (!valid_dbus_call_names(call) ||
        !dbus_valid_signature(call->signature, false)) {
        return luaL_error(lua, "invalid D-Bus call name, path, or signature");
    }

    lua_getfield(lua, 2, "args");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        lua_newtable(lua);
    } else {
        luaL_checktype(lua, -1, LUA_TTABLE);
    }
    const char *validation_error = NULL;
    size_t value_count = 0;
    if (!validate_dbus_sequence(lua, call->signature,
                                call->signature + strlen(call->signature), -1,
                                0, &value_count, &validation_error)) {
        return luaL_error(lua, "%s", validation_error);
    }
    call->args_ref = luaL_ref(lua, LUA_REGISTRYINDEX);

    lua_getfield(lua, 2, "timeout");
    if (!lua_isnil(lua, -1)) {
        const lua_Number seconds = luaL_checknumber(lua, -1);
        if (!isfinite(seconds) || seconds <= 0 ||
            seconds > (lua_Number)UINT64_MAX / 1000000.0) {
            return luaL_error(lua, "D-Bus timeout must be finite and positive");
        }
        call->timeout_us = (uint64_t)ceill(seconds * 1000000.0);
    }
    lua_pop(lua, 1);
    lua_pushvalue(lua, 3);
    call->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    call->next = generation->dbus_calls;
    generation->dbus_calls = call;
    generation->dbus_operation_count++;

    struct lua_dbus_handle *handle = new_dbus_handle(lua, LUA_DBUS_CALL);
    handle->value.call.generation = generation;
    handle->value.call.id = call->id;
    return 1;
}

static int lua_dbus_on_connect(lua_State *lua) {
    struct lua_dbus_bus *bus = check_dbus_bus(lua, 1);
    struct i3sd_dbus_generation *generation = bus->block->generation;
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    if (generation->dbus_operation_count == I3SD_DBUS_OPERATION_LIMIT) {
        return luaL_error(lua, "D-Bus operation limit reached");
    }
    struct dbus_connect_watch *watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return luaL_error(lua, "out of memory creating D-Bus connect watch");
    }
    watch->block = bus->block;
    watch->scope = bus->scope;
    lua_pushvalue(lua, 2);
    watch->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    watch->next = generation->dbus_connect_watches;
    generation->dbus_connect_watches = watch;
    generation->dbus_operation_count++;

    struct lua_dbus_handle *handle =
        new_dbus_handle(lua, LUA_DBUS_CONNECT_WATCH);
    handle->value.connect_watch = watch;
    return 1;
}

static struct lua_dbus_message *check_dbus_message(lua_State *lua) {
    struct lua_dbus_message *message =
        luaL_checkudata(lua, 1, dbus_message_metatable);
    if (!message->valid || message->message == NULL) {
        luaL_error(lua, "D-Bus message is no longer valid");
    }
    return message;
}

static int lua_dbus_message_read(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    const char *signature = luaL_checkstring(lua, 2);
    if (!dbus_valid_signature(signature, false)) {
        return luaL_error(lua, "invalid D-Bus signature");
    }
    const size_t requested_len = strlen(signature);
    if (view->consumed_len + requested_len > 255) {
        lua_pushnil(lua);
        push_error(lua, "invalid_data", "D-Bus read exceeds message signature",
                   "dbus", 0);
        return 2;
    }
    const char *remaining = sd_bus_message_get_signature(view->message, true);
    if (remaining == NULL ||
        strncmp(remaining, signature, requested_len) != 0) {
        lua_pushnil(lua);
        push_error(lua, "type_mismatch", "D-Bus reply signature mismatch",
                   "dbus", 0);
        return 2;
    }

    const int base = lua_gettop(lua);
    size_t value_count = 0;
    int results = 0;
    bool valid = true;
    for (const char *cursor = signature; *cursor != '\0'; results++) {
        const char *next = dbus_one_type_end(cursor);
        if (!decode_dbus_value(view->message, lua, cursor, next, 0,
                               &value_count)) {
            valid = false;
            break;
        }
        cursor = next;
    }
    if (!valid) {
        /* Rewind and replay prior successful reads to keep failure atomic. */
        lua_settop(lua, base);
        sd_bus_message_rewind(view->message, true);
        if (view->consumed_len > 0) {
            view->consumed[view->consumed_len] = '\0';
            sd_bus_message_skip(view->message, view->consumed);
        }
        lua_pushnil(lua);
        push_error(lua, "invalid_data", "unable to decode D-Bus message",
                   "dbus", 0);
        return 2;
    }
    memcpy(view->consumed + view->consumed_len, signature, requested_len);
    view->consumed_len += requested_len;
    view->consumed[view->consumed_len] = '\0';
    return results;
}

static int lua_dbus_message_property(lua_State *lua, const char *value) {
    check_dbus_message(lua);
    if (value == NULL) {
        lua_pushnil(lua);
    } else {
        lua_pushstring(lua, value);
    }
    return 1;
}

static int lua_dbus_message_sender(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    return lua_dbus_message_property(lua,
                                     sd_bus_message_get_sender(view->message));
}

static int lua_dbus_message_path(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    return lua_dbus_message_property(lua,
                                     sd_bus_message_get_path(view->message));
}

static int lua_dbus_message_interface(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    return lua_dbus_message_property(
        lua, sd_bus_message_get_interface(view->message));
}

static int lua_dbus_message_member(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    return lua_dbus_message_property(lua,
                                     sd_bus_message_get_member(view->message));
}

static int lua_dbus_message_signature(lua_State *lua) {
    struct lua_dbus_message *view = check_dbus_message(lua);
    return lua_dbus_message_property(
        lua, sd_bus_message_get_signature(view->message, true));
}

static int lua_dbus_variant_new(lua_State *lua) {
    const char *signature = luaL_checkstring(lua, 1);
    if (!dbus_valid_signature(signature, true)) {
        return luaL_error(lua, "D-Bus variant signature must contain one type");
    }
    size_t count = 0;
    const char *error = NULL;
    if (!validate_dbus_value(lua, signature, signature + strlen(signature), 2,
                             0, &count, &error)) {
        return luaL_error(lua, "%s", error);
    }
    struct lua_dbus_variant *variant = lua_newuserdata(lua, sizeof(*variant));
    variant->signature = strdup(signature);
    if (variant->signature == NULL) {
        return luaL_error(lua, "out of memory creating D-Bus variant");
    }
    lua_pushvalue(lua, 2);
    variant->value_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    luaL_getmetatable(lua, dbus_variant_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

static int lua_dbus_variant_signature(lua_State *lua) {
    struct lua_dbus_variant *variant =
        luaL_checkudata(lua, 1, dbus_variant_metatable);
    lua_pushstring(lua, variant->signature);
    return 1;
}

static int lua_dbus_variant_value(lua_State *lua) {
    struct lua_dbus_variant *variant =
        luaL_checkudata(lua, 1, dbus_variant_metatable);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, variant->value_ref);
    return 1;
}

static int lua_dbus_variant_destroy(lua_State *lua) {
    struct lua_dbus_variant *variant =
        luaL_checkudata(lua, 1, dbus_variant_metatable);
    free(variant->signature);
    variant->signature = NULL;
    if (variant->value_ref != LUA_NOREF) {
        luaL_unref(lua, LUA_REGISTRYINDEX, variant->value_ref);
        variant->value_ref = LUA_NOREF;
    }
    return 0;
}

static int lua_dbus_dict(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    const size_t length = lua_objlen(lua, 1);
    if (!lua_sequence_is_dense(lua, 1, length)) {
        return luaL_error(lua,
                          "D-Bus dictionary must be a dense pair sequence");
    }
    for (size_t item = 1; item <= length; item++) {
        lua_rawgeti(lua, 1, (int)item);
        if (lua_type(lua, -1) != LUA_TTABLE || lua_objlen(lua, -1) != 2 ||
            !lua_sequence_is_dense(lua, -1, 2)) {
            return luaL_error(lua, "D-Bus dictionary entries must be pairs");
        }
        lua_pop(lua, 1);
    }
    lua_settop(lua, 1);
    return 1;
}

static bool decode_dbus_value(sd_bus_message *message, lua_State *lua,
                              const char *start, const char *end,
                              unsigned depth, size_t *value_count) {
    if (depth > I3SD_DBUS_CONTAINER_DEPTH ||
        ++*value_count > I3SD_DBUS_VALUE_LIMIT) {
        return false;
    }
    const char type = *start;
    int result;
    if (type == 'y') {
        uint8_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushinteger(lua, value);
    } else if (type == 'b') {
        int value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushboolean(lua, value);
    } else if (type == 'n') {
        int16_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushinteger(lua, value);
    } else if (type == 'q') {
        uint16_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushinteger(lua, value);
    } else if (type == 'i') {
        int32_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushinteger(lua, value);
    } else if (type == 'u') {
        uint32_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushnumber(lua, value);
    } else if (type == 'x') {
        int64_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        dbus_push_int64(lua, value);
    } else if (type == 't') {
        uint64_t value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        dbus_push_uint64(lua, value);
    } else if (type == 'd') {
        double value = 0;
        result = sd_bus_message_read_basic(message, type, &value);
        lua_pushnumber(lua, value);
    } else if (type == 's' || type == 'o' || type == 'g') {
        const char *value = NULL;
        result = sd_bus_message_read_basic(message, type, &value);
        if (result <= 0 || value == NULL ||
            strlen(value) > I3SD_DBUS_MESSAGE_LIMIT ||
            (type == 's' && !i3sd_valid_utf8(value, strlen(value)))) {
            return false;
        }
        lua_pushstring(lua, value);
    } else if (type == 'v') {
        char actual_type;
        const char *contents;
        if (sd_bus_message_peek_type(message, &actual_type, &contents) <= 0 ||
            actual_type != 'v' || contents == NULL ||
            !dbus_valid_signature(contents, true) ||
            sd_bus_message_enter_container(message, 'v', contents) <= 0) {
            return false;
        }
        const char *variant_end = contents + strlen(contents);
        if (!decode_dbus_value(message, lua, contents, variant_end, depth + 1,
                               value_count) ||
            sd_bus_message_exit_container(message) < 0) {
            return false;
        }
        struct lua_dbus_variant *variant =
            lua_newuserdata(lua, sizeof(*variant));
        variant->signature = strdup(contents);
        lua_insert(lua, -2);
        variant->value_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
        if (variant->signature == NULL) {
            return false;
        }
        luaL_getmetatable(lua, dbus_variant_metatable);
        lua_setmetatable(lua, -2);
        return true;
    } else {
        const char *content_start = start + 1;
        const char *content_end = type == 'a' ? end : end - 1;
        char contents[256];
        const size_t content_len = (size_t)(content_end - content_start);
        memcpy(contents, content_start, content_len);
        contents[content_len] = '\0';
        if (type == 'a' && *content_start == 'y' && content_start + 1 == end) {
            const void *bytes;
            size_t length;
            result = sd_bus_message_read_array(message, 'y', &bytes, &length);
            if (result > 0 && length <= I3SD_DBUS_MESSAGE_LIMIT) {
                lua_pushlstring(lua, bytes, length);
            }
            return result > 0 && length <= I3SD_DBUS_MESSAGE_LIMIT;
        }
        const char container_type = type == 'a' ? 'a' : type == '(' ? 'r' : 'e';
        if (sd_bus_message_enter_container(message, container_type, contents) <=
            0) {
            return false;
        }
        if (type == 'a') {
            lua_newtable(lua);
            int item = 1;
            char next_type;
            while ((result = sd_bus_message_peek_type(message, &next_type,
                                                      NULL)) > 0) {
                if (!decode_dbus_value(message, lua, content_start, content_end,
                                       depth + 1, value_count)) {
                    lua_pop(lua, 1);
                    return false;
                }
                lua_rawseti(lua, -2, item++);
            }
            if (result < 0) {
                lua_pop(lua, 1);
                return false;
            }
        } else if (!decode_dbus_sequence(message, lua, content_start,
                                         content_end, depth + 1, value_count)) {
            return false;
        }
        return sd_bus_message_exit_container(message) >= 0;
    }
    if (result <= 0) {
        lua_pop(lua, 1);
        return false;
    }
    return true;
}

static bool dbus_callback_allowed(const struct dbus_owner *block) {
    return !block->faulted &&
           block->generation->host.owner_active(block->userdata);
}

static bool finish_dbus_callback(struct dbus_owner *block, int arguments,
                                 const char *phase) {
    lua_State *lua = block->generation->lua;
    if (lua_pcall(lua, arguments, 0, 0) != 0) {
        log_dbus_error(block, phase);
        fault_dbus_owner(block);
        return false;
    }
    return true;
}

static void push_dbus_error(lua_State *lua, const char *code,
                            const char *message, const char *dbus_name,
                            int error_number, bool transient) {
    push_error(lua, code, message, "dbus", error_number);
    if (dbus_name != NULL) {
        lua_pushstring(lua, dbus_name);
        lua_setfield(lua, -2, "dbus_name");
    }
    if (transient) {
        lua_pushboolean(lua, true);
        lua_setfield(lua, -2, "transient");
    }
}

static void invoke_dbus_call_error(struct dbus_call *call, const char *code,
                                   const char *message, const char *dbus_name,
                                   int error_number, bool transient) {
    if (!dbus_callback_allowed(call->block) || call->cancelled ||
        call->completed) {
        return;
    }
    call->completed = true;
    lua_State *lua = call->block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, call->callback_ref);
    lua_pushnil(lua);
    push_dbus_error(lua, code, message, dbus_name, error_number, transient);
    finish_dbus_callback(call->block, 2, "D-Bus call callback");
}

static int dbus_match_message(sd_bus_message *message, void *userdata,
                              sd_bus_error *ret_error) {
    (void)ret_error;
    struct dbus_match *match = userdata;
    if (!dbus_callback_allowed(match->block) || match->cancelled) {
        return 0;
    }
    lua_State *lua = match->block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, match->callback_ref);
    struct lua_dbus_message *view = lua_newuserdata(lua, sizeof(*view));
    *view = (struct lua_dbus_message){
        .message = message,
        .valid = true,
    };
    luaL_getmetatable(lua, dbus_message_metatable);
    lua_setmetatable(lua, -2);
    const bool success =
        finish_dbus_callback(match->block, 1, "D-Bus match callback");
    view->valid = false;
    view->message = NULL;
    return success ? 0 : -ECANCELED;
}

static int dbus_match_installed(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error) {
    (void)ret_error;
    struct dbus_match *match = userdata;
    if (!dbus_callback_allowed(match->block) || match->cancelled ||
        match->install_ref == LUA_NOREF) {
        return 0;
    }
    lua_State *lua = match->block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, match->install_ref);
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *error = sd_bus_message_get_error(message);
        push_dbus_error(lua, "dbus_error",
                        error != NULL && error->message != NULL
                            ? error->message
                            : "D-Bus match installation failed",
                        error == NULL ? NULL : error->name, 0, false);
    } else {
        lua_pushnil(lua);
    }
    finish_dbus_callback(match->block, 1, "D-Bus match install callback");
    return 0;
}

static int dbus_call_completed(sd_bus_message *message, void *userdata,
                               sd_bus_error *ret_error) {
    (void)ret_error;
    struct dbus_call *call = userdata;
    call->slot = sd_bus_slot_unref(call->slot);
    if (!dbus_callback_allowed(call->block) || call->cancelled ||
        call->completed) {
        return 0;
    }
    call->completed = true;
    lua_State *lua = call->block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, call->callback_ref);
    if (sd_bus_message_is_method_error(message, NULL)) {
        const sd_bus_error *error = sd_bus_message_get_error(message);
        lua_pushnil(lua);
        push_dbus_error(lua, "dbus_error",
                        error != NULL && error->message != NULL
                            ? error->message
                            : "D-Bus method call failed",
                        error == NULL ? NULL : error->name, 0, false);
        finish_dbus_callback(call->block, 2, "D-Bus call callback");
        return 0;
    }

    struct lua_dbus_message *view = lua_newuserdata(lua, sizeof(*view));
    *view = (struct lua_dbus_message){
        .message = message,
        .valid = true,
    };
    luaL_getmetatable(lua, dbus_message_metatable);
    lua_setmetatable(lua, -2);
    lua_pushnil(lua);
    const bool success =
        finish_dbus_callback(call->block, 2, "D-Bus call callback");
    view->valid = false;
    view->message = NULL;
    return success ? 0 : -ECANCELED;
}

static bool dbus_transport_needed(const struct i3sd_dbus_runtime *app,
                                  enum dbus_scope scope) {
    if (app->current == NULL) {
        return false;
    }
    for (const struct dbus_match *match = app->current->dbus_matches;
         match != NULL; match = match->next) {
        if (!match->cancelled && match->scope == scope) {
            return true;
        }
    }
    for (const struct dbus_call *call = app->current->dbus_calls; call != NULL;
         call = call->next) {
        if (!call->cancelled && !call->completed && call->scope == scope) {
            return true;
        }
    }
    for (const struct dbus_connect_watch *watch =
             app->current->dbus_connect_watches;
         watch != NULL; watch = watch->next) {
        if (!watch->cancelled && watch->scope == scope) {
            return true;
        }
    }
    return false;
}

static void reap_dbus_calls(struct i3sd_dbus_generation *generation) {
    if (generation == NULL) {
        return;
    }
    struct dbus_call **link = &generation->dbus_calls;
    while (*link != NULL) {
        struct dbus_call *call = *link;
        if ((!call->completed && !call->cancelled) || call->slot != NULL) {
            link = &call->next;
            continue;
        }
        *link = call->next;
        luaL_unref(generation->lua, LUA_REGISTRYINDEX, call->args_ref);
        luaL_unref(generation->lua, LUA_REGISTRYINDEX, call->callback_ref);
        free(call->destination);
        free(call->path);
        free(call->interface);
        free(call->member);
        free(call->signature);
        free(call);
        generation->dbus_operation_count--;
    }
}

static void reset_dbus_slots(struct i3sd_dbus_generation *generation,
                             enum dbus_scope scope, bool disconnected) {
    /* Native slots belong to one connection epoch; logical matches survive. */
    if (generation == NULL) {
        return;
    }
    for (struct dbus_match *match = generation->dbus_matches; match != NULL;
         match = match->next) {
        if (match->scope == scope) {
            match->slot = sd_bus_slot_unref(match->slot);
            match->attempted_epoch = 0;
        }
    }
    for (struct dbus_call *call = generation->dbus_calls; call != NULL;
         call = call->next) {
        if (call->scope != scope) {
            continue;
        }
        call->slot = sd_bus_slot_unref(call->slot);
        if (disconnected && call->sent && !call->completed &&
            !call->cancelled) {
            invoke_dbus_call_error(call, "disconnected",
                                   "D-Bus connection was lost", NULL,
                                   ECONNRESET, true);
        }
    }
}

static void close_dbus_transport(struct dbus_transport *transport,
                                 bool disconnected) {
    if (transport->registered_fd >= 0) {
        epoll_ctl(transport->app->epoll_fd, EPOLL_CTL_DEL,
                  transport->registered_fd, NULL);
    }
    transport->registered_fd = -1;
    transport->registered_events = 0;
    reset_dbus_slots(transport->app->current, transport->scope, disconnected);
    reap_dbus_calls(transport->app->current);
    transport->bus = sd_bus_flush_close_unref(transport->bus);
}

static bool open_dbus_transport(struct dbus_transport *transport,
                                uint64_t now_ns) {
    int result = transport->scope == DBUS_SCOPE_SYSTEM
                     ? sd_bus_open_system(&transport->bus)
                     : sd_bus_open_user(&transport->bus);
    if (result < 0) {
        transport->bus = sd_bus_unref(transport->bus);
        transport->failed_count++;
        const uint32_t shift =
            transport->failed_count > 6 ? 6 : transport->failed_count;
        transport->retry_deadline_ns =
            now_ns + ((uint64_t)1 << shift) * 100000000ULL;
        return false;
    }
    sd_bus_set_exit_on_disconnect(transport->bus, 0);
    sd_bus_set_watch_bind(transport->bus, 1);
    transport->failed_count = 0;
    transport->retry_deadline_ns = 0;
    transport->epoch++;
    if (transport->epoch == 0) {
        fprintf(stderr, "i3sd: D-Bus connection epoch exhausted\n");
        exit(EXIT_FAILURE);
    }
    return true;
}

static void invoke_dbus_connect_watch(struct dbus_connect_watch *watch,
                                      uint64_t epoch) {
    if (!dbus_callback_allowed(watch->block) || watch->cancelled ||
        watch->delivered_epoch == epoch) {
        return;
    }
    watch->delivered_epoch = epoch;
    lua_State *lua = watch->block->generation->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, watch->callback_ref);
    finish_dbus_callback(watch->block, 0, "D-Bus connect callback");
}

static void start_dbus_call(struct dbus_transport *transport,
                            struct dbus_call *call) {
    sd_bus_message *message = NULL;
    int result = sd_bus_message_new_method_call(transport->bus, &message,
                                                call->destination, call->path,
                                                call->interface, call->member);
    if (result >= 0) {
        lua_State *lua = call->block->generation->lua;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, call->args_ref);
        result =
            encode_dbus_sequence(message, lua, call->signature,
                                 call->signature + strlen(call->signature), -1);
        lua_pop(lua, 1);
    }
    call->sent = true;
    if (result >= 0) {
        result = sd_bus_call_async(transport->bus, &call->slot, message,
                                   dbus_call_completed, call, call->timeout_us);
    }
    sd_bus_message_unref(message);
    if (result < 0) {
        invoke_dbus_call_error(call, "send_failed", strerror(-result), NULL,
                               -result, result == -ENOTCONN);
    }
}

static void activate_dbus_operations(struct dbus_transport *transport) {
    struct i3sd_dbus_generation *generation = transport->app->current;
    if (generation == NULL || transport->bus == NULL) {
        return;
    }
    for (struct dbus_match *match = generation->dbus_matches; match != NULL;
         match = match->next) {
        if (match->cancelled || match->scope != transport->scope ||
            match->attempted_epoch == transport->epoch) {
            continue;
        }
        match->attempted_epoch = transport->epoch;
        const int result = sd_bus_add_match_async(
            transport->bus, &match->slot, match->rule, dbus_match_message,
            dbus_match_installed, match);
        if (result < 0 && match->install_ref != LUA_NOREF &&
            dbus_callback_allowed(match->block)) {
            lua_State *lua = match->block->generation->lua;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, match->install_ref);
            push_dbus_error(lua, "install_failed", strerror(-result), NULL,
                            -result, false);
            finish_dbus_callback(match->block, 1,
                                 "D-Bus match install callback");
        }
    }
    for (struct dbus_call *call = generation->dbus_calls; call != NULL;
         call = call->next) {
        if (!call->cancelled && !call->completed && !call->sent &&
            call->scope == transport->scope) {
            start_dbus_call(transport, call);
        }
    }
    for (struct dbus_connect_watch *watch = generation->dbus_connect_watches;
         watch != NULL; watch = watch->next) {
        if (watch->scope == transport->scope) {
            invoke_dbus_connect_watch(watch, transport->epoch);
        }
    }
}

static void reconcile_dbus_transport(struct dbus_transport *transport,
                                     uint64_t now_ns) {
    reap_dbus_calls(transport->app->current);
    if (!dbus_transport_needed(transport->app, transport->scope)) {
        if (transport->bus != NULL) {
            close_dbus_transport(transport, false);
        }
        return;
    }
    if (transport->bus == NULL) {
        if (transport->retry_deadline_ns > now_ns ||
            !open_dbus_transport(transport, now_ns)) {
            return;
        }
    }
    activate_dbus_operations(transport);
    reap_dbus_calls(transport->app->current);

    const int fd = sd_bus_get_fd(transport->bus);
    const int poll_events = sd_bus_get_events(transport->bus);
    if (fd < 0 || poll_events < 0) {
        close_dbus_transport(transport, true);
        return;
    }
    uint32_t events = EPOLLERR | EPOLLHUP;
    if ((poll_events & POLLIN) != 0) {
        events |= EPOLLIN;
    }
    if ((poll_events & POLLOUT) != 0) {
        events |= EPOLLOUT;
    }
    if (transport->registered_fd == fd &&
        transport->registered_events == events) {
        return;
    }
    if (transport->registered_fd >= 0) {
        epoll_ctl(transport->app->epoll_fd, EPOLL_CTL_DEL,
                  transport->registered_fd, NULL);
        transport->registered_fd = -1;
    }
    struct epoll_event event = {
        .events = events,
    };
    (*transport->app->next_registration_cookie)++;
    if (*transport->app->next_registration_cookie == 0) {
        fprintf(stderr, "i3sd: epoll registration cookie exhausted\n");
        exit(EXIT_FAILURE);
    }
    transport->cookie = *transport->app->next_registration_cookie;
    event.data.u64 = transport->cookie;
    if (epoll_ctl(transport->app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        close_dbus_transport(transport, true);
        return;
    }
    transport->registered_fd = fd;
    transport->registered_events = events;
}

static void process_dbus_transport(struct dbus_transport *transport,
                                   uint64_t now_ns) {
    if (transport->bus == NULL) {
        return;
    }
    for (size_t count = 0; count < I3SD_DBUS_PROCESS_BUDGET; count++) {
        const int result = sd_bus_process(transport->bus, NULL);
        if (result < 0) {
            close_dbus_transport(transport, true);
            return;
        }
        if (result == 0) {
            break;
        }
    }
    reconcile_dbus_transport(transport, now_ns);
}

void i3sd_dbus_cancel_owner(struct i3sd_dbus_generation *generation,
                            void *owner) {
    if (generation == NULL) {
        return;
    }
    for (struct dbus_owner *entry = generation->owners; entry != NULL;
         entry = entry->next) {
        if (entry->userdata == owner) {
            entry->faulted = true;
        }
    }
    for (struct dbus_match *match = generation->dbus_matches; match != NULL;
         match = match->next) {
        if (match->block->userdata == owner) {
            match->cancelled = true;
            match->slot = sd_bus_slot_unref(match->slot);
        }
    }
    for (struct dbus_call *call = generation->dbus_calls; call != NULL;
         call = call->next) {
        if (call->block->userdata == owner) {
            call->cancelled = true;
            call->slot = sd_bus_slot_unref(call->slot);
        }
    }
    for (struct dbus_connect_watch *watch = generation->dbus_connect_watches;
         watch != NULL; watch = watch->next) {
        if (watch->block->userdata == owner) {
            watch->cancelled = true;
        }
    }
}

static void fault_dbus_owner(struct dbus_owner *owner) {
    if (owner->faulted) {
        return;
    }
    i3sd_dbus_cancel_owner(owner->generation, owner->userdata);
    owner->generation->host.fault_owner(owner->userdata);
}

struct i3sd_dbus_generation *
i3sd_dbus_generation_create(lua_State *lua, const struct i3sd_dbus_host *host) {
    struct i3sd_dbus_generation *generation = calloc(1, sizeof(*generation));
    if (generation == NULL) {
        return NULL;
    }
    generation->lua = lua;
    generation->host = *host;
    lua_pushlightuserdata(lua, generation);
    lua_setfield(lua, LUA_REGISTRYINDEX, "i3sd.dbus.generation");
    return generation;
}

void i3sd_dbus_generation_deactivate(struct i3sd_dbus_generation *generation) {
    if (generation == NULL) {
        return;
    }
    for (struct dbus_match *match = generation->dbus_matches; match != NULL;
         match = match->next) {
        match->cancelled = true;
        match->slot = sd_bus_slot_unref(match->slot);
    }
    for (struct dbus_call *call = generation->dbus_calls; call != NULL;
         call = call->next) {
        call->cancelled = true;
        call->slot = sd_bus_slot_unref(call->slot);
    }
    for (struct dbus_connect_watch *watch = generation->dbus_connect_watches;
         watch != NULL; watch = watch->next) {
        watch->cancelled = true;
    }
}

void i3sd_dbus_generation_destroy(struct i3sd_dbus_generation *generation) {
    if (generation == NULL) {
        return;
    }
    struct dbus_match *match = generation->dbus_matches;
    while (match != NULL) {
        struct dbus_match *next = match->next;
        free(match->rule);
        free(match);
        match = next;
    }
    struct dbus_call *call = generation->dbus_calls;
    while (call != NULL) {
        struct dbus_call *next = call->next;
        free(call->destination);
        free(call->path);
        free(call->interface);
        free(call->member);
        free(call->signature);
        free(call);
        call = next;
    }
    struct dbus_connect_watch *watch = generation->dbus_connect_watches;
    while (watch != NULL) {
        struct dbus_connect_watch *next = watch->next;
        free(watch);
        watch = next;
    }
    struct dbus_owner *owner = generation->owners;
    while (owner != NULL) {
        struct dbus_owner *next = owner->next;
        free(owner);
        owner = next;
    }
    free(generation);
}

void i3sd_dbus_register_lua(struct i3sd_dbus_generation *generation,
                            int i3sd_table) {
    lua_State *lua = generation->lua;
    i3sd_table = absolute_lua_index(lua, i3sd_table);

    luaL_newmetatable(lua, dbus_bus_metatable);
    lua_newtable(lua);
    static const luaL_Reg bus_methods[] = {
        {"match", lua_dbus_match},
        {"call", lua_dbus_call},
        {"on_connect", lua_dbus_on_connect},
        {NULL, NULL},
    };
    luaL_register(lua, NULL, bus_methods);
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, dbus_handle_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_dbus_handle_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_dbus_handle_cancel);
    lua_setfield(lua, -2, "__gc");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, dbus_message_metatable);
    lua_newtable(lua);
    static const luaL_Reg message_methods[] = {
        {"read", lua_dbus_message_read},
        {"sender", lua_dbus_message_sender},
        {"path", lua_dbus_message_path},
        {"interface", lua_dbus_message_interface},
        {"member", lua_dbus_message_member},
        {"signature", lua_dbus_message_signature},
        {NULL, NULL},
    };
    luaL_register(lua, NULL, message_methods);
    lua_setfield(lua, -2, "__index");
    lua_pop(lua, 1);

    luaL_newmetatable(lua, dbus_variant_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_dbus_variant_signature);
    lua_setfield(lua, -2, "signature");
    lua_pushcfunction(lua, lua_dbus_variant_value);
    lua_setfield(lua, -2, "value");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_dbus_variant_destroy);
    lua_setfield(lua, -2, "__gc");
    lua_pop(lua, 1);

    lua_newtable(lua);
    lua_pushcfunction(lua, lua_dbus_variant_new);
    lua_setfield(lua, -2, "variant");
    lua_pushcfunction(lua, lua_dbus_dict);
    lua_setfield(lua, -2, "dict");
    lua_setfield(lua, i3sd_table, "dbus");
}

int i3sd_dbus_push_bus(lua_State *lua, struct i3sd_dbus_generation *generation,
                       void *userdata, const char *scope) {
    enum dbus_scope parsed;
    if (strcmp(scope, "system") == 0) {
        parsed = DBUS_SCOPE_SYSTEM;
    } else if (strcmp(scope, "user") == 0) {
        parsed = DBUS_SCOPE_USER;
    } else {
        return luaL_error(lua, "D-Bus scope must be system or user");
    }

    struct dbus_owner *owner = generation->owners;
    while (owner != NULL && owner->userdata != userdata) {
        owner = owner->next;
    }
    if (owner == NULL) {
        owner = calloc(1, sizeof(*owner));
        if (owner == NULL) {
            return luaL_error(lua, "out of memory creating D-Bus owner");
        }
        owner->generation = generation;
        owner->userdata = userdata;
        owner->next = generation->owners;
        generation->owners = owner;
    }

    struct lua_dbus_bus *bus = lua_newuserdata(lua, sizeof(*bus));
    bus->block = owner;
    bus->scope = parsed;
    luaL_getmetatable(lua, dbus_bus_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

struct i3sd_dbus_runtime *
i3sd_dbus_runtime_create(int epoll_fd, uint64_t *next_registration_cookie) {
    struct i3sd_dbus_runtime *runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        return NULL;
    }
    runtime->epoll_fd = epoll_fd;
    runtime->next_registration_cookie = next_registration_cookie;
    runtime->transports[0] = (struct dbus_transport){
        .app = runtime,
        .registered_fd = -1,
        .scope = DBUS_SCOPE_SYSTEM,
    };
    runtime->transports[1] = (struct dbus_transport){
        .app = runtime,
        .registered_fd = -1,
        .scope = DBUS_SCOPE_USER,
    };
    return runtime;
}

void i3sd_dbus_runtime_destroy(struct i3sd_dbus_runtime *runtime) {
    if (runtime == NULL) {
        return;
    }
    for (size_t index = 0; index < 2; index++) {
        if (runtime->transports[index].bus != NULL) {
            close_dbus_transport(&runtime->transports[index], false);
        }
    }
    free(runtime);
}

void i3sd_dbus_reconcile(struct i3sd_dbus_runtime *runtime,
                         struct i3sd_dbus_generation *generation,
                         uint64_t now_ns) {
    runtime->current = generation;
    reconcile_dbus_transport(&runtime->transports[0], now_ns);
    reconcile_dbus_transport(&runtime->transports[1], now_ns);
}

bool i3sd_dbus_process_cookie(struct i3sd_dbus_runtime *runtime,
                              struct i3sd_dbus_generation *generation,
                              uint64_t cookie, uint64_t now_ns) {
    runtime->current = generation;
    for (size_t index = 0; index < 2; index++) {
        struct dbus_transport *transport = &runtime->transports[index];
        if (transport->registered_fd >= 0 && transport->cookie == cookie) {
            process_dbus_transport(transport, now_ns);
            return true;
        }
    }
    return false;
}

void i3sd_dbus_process(struct i3sd_dbus_runtime *runtime,
                       struct i3sd_dbus_generation *generation,
                       uint64_t now_ns) {
    runtime->current = generation;
    process_dbus_transport(&runtime->transports[0], now_ns);
    process_dbus_transport(&runtime->transports[1], now_ns);
}

uint64_t i3sd_dbus_deadline(const struct i3sd_dbus_runtime *runtime,
                            const struct i3sd_dbus_generation *generation) {
    (void)generation;
    uint64_t deadline = UINT64_MAX;
    for (size_t index = 0; index < 2; index++) {
        const struct dbus_transport *transport = &runtime->transports[index];
        if (!dbus_transport_needed(runtime, transport->scope)) {
            continue;
        }
        if (transport->bus == NULL) {
            if (transport->retry_deadline_ns != 0 &&
                transport->retry_deadline_ns < deadline) {
                deadline = transport->retry_deadline_ns;
            }
            continue;
        }
        uint64_t timeout_us;
        if (sd_bus_get_timeout(transport->bus, &timeout_us) >= 0 &&
            timeout_us != UINT64_MAX) {
            const uint64_t timeout_ns =
                timeout_us > UINT64_MAX / 1000 ? UINT64_MAX : timeout_us * 1000;
            if (timeout_ns < deadline) {
                deadline = timeout_ns;
            }
        }
    }
    return deadline;
}
