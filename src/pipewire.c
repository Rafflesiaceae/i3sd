#define _GNU_SOURCE

#include "pipewire.h"
#include "config.h"

#include <lauxlib.h>
#include <stdlib.h>

#if I3SD_HAVE_PIPEWIRE

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#pragma GCC diagnostic pop
#include <yyjson.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>

#define I3SD_PIPEWIRE_SINK_LIMIT 256U
#define I3SD_PIPEWIRE_NAME_LIMIT 511U
#define I3SD_PIPEWIRE_DISPATCH_BUDGET 64U
#define I3SD_PIPEWIRE_RETRY_NS 5000000000ULL

struct pipewire_sink {
    uint32_t id;
    uint32_t version;
    char name[I3SD_PIPEWIRE_NAME_LIMIT + 1];
};

struct pipewire_subscription {
    struct pipewire_subscription *next;
    struct block *block;
    int callback_ref;
    uint64_t id;
    uint64_t delivered_revision;
};

struct lua_pipewire_handle {
    struct i3sd_pipewire_source *source;
    uint64_t subscription_id;
};

struct i3sd_pipewire_source {
    struct app *app;
    struct pw_main_loop *main_loop;
    struct pw_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct pw_node *node;
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;
    struct spa_hook node_listener;
    struct pipewire_sink sinks[I3SD_PIPEWIRE_SINK_LIMIT];
    size_t sink_count;
    struct pipewire_subscription *subscriptions;
    uint64_t retry_deadline_ns;
    uint64_t next_subscription_id;
    uint64_t state_revision;
    uint32_t metadata_id;
    uint32_t node_id;
    int registered_fd;
    double volume;
    char default_sink[I3SD_PIPEWIRE_NAME_LIMIT + 1];
    bool loop_entered;
    bool state_valid;
    bool muted;
    bool reconnect_pending;
};

static const char handle_metatable[] = "i3sd.pipewire_handle";

static bool subscription_active(const struct pipewire_subscription *entry) {
    const struct block *block = entry->block;
    return !block->faulted &&
           block->generation->app->current == block->generation;
}

static bool source_needed(const struct i3sd_pipewire_source *source) {
    for (const struct pipewire_subscription *entry = source->subscriptions;
         entry != NULL; entry = entry->next) {
        if (subscription_active(entry)) {
            return true;
        }
    }
    return false;
}

static void notify_subscription(struct i3sd_pipewire_source *source,
                                struct pipewire_subscription *entry) {
    struct block *block = entry->block;
    lua_State *lua = block->generation->lua;
    entry->delivered_revision = source->state_revision;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, entry->callback_ref);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
    lua_newtable(lua);
    lua_pushboolean(lua, source->state_valid);
    lua_setfield(lua, -2, "available");
    if (source->state_valid) {
        lua_pushnumber(lua, source->volume);
        lua_setfield(lua, -2, "volume");
        lua_pushboolean(lua, source->muted);
        lua_setfield(lua, -2, "muted");
    }
    if (lua_pcall(lua, 2, 0, 0) != 0) {
        i3sd_log_lua_error(block, "PipeWire event");
        i3sd_fault_block(block);
    }
}

void i3sd_pipewire_notify(struct i3sd_pipewire_source *source) {
    if (source == NULL) {
        return;
    }
    struct pipewire_subscription *entry = source->subscriptions;
    while (entry != NULL) {
        /* A failed callback may remove its own subscription. */
        struct pipewire_subscription *next = entry->next;
        if (subscription_active(entry) &&
            entry->delivered_revision != source->state_revision) {
            notify_subscription(source, entry);
        }
        entry = next;
    }
}

static void publish_unavailable(struct i3sd_pipewire_source *source) {
    if (!source->state_valid) {
        return;
    }
    source->state_valid = false;
    source->state_revision++;
    i3sd_pipewire_notify(source);
}

static void publish_volume(struct i3sd_pipewire_source *source, double volume,
                           bool muted) {
    if (source->state_valid && fabs(source->volume - volume) < 0.000001 &&
        source->muted == muted) {
        return;
    }
    source->volume = volume;
    source->muted = muted;
    source->state_valid = true;
    source->state_revision++;
    i3sd_pipewire_notify(source);
}

static void detach_node(struct i3sd_pipewire_source *source) {
    if (source->node != NULL) {
        spa_hook_remove(&source->node_listener);
        pw_proxy_destroy((struct pw_proxy *)source->node);
        source->node = NULL;
    }
    source->node_id = SPA_ID_INVALID;
}

static void detach_metadata(struct i3sd_pipewire_source *source) {
    if (source->metadata != NULL) {
        spa_hook_remove(&source->metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)source->metadata);
        source->metadata = NULL;
    }
    source->metadata_id = SPA_ID_INVALID;
}

static void close_native(struct i3sd_pipewire_source *source) {
    if (source->registered_fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->registered_fd,
                  NULL);
        source->registered_fd = -1;
    }
    detach_node(source);
    detach_metadata(source);
    if (source->registry != NULL) {
        spa_hook_remove(&source->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)source->registry);
        source->registry = NULL;
    }
    if (source->core != NULL) {
        spa_hook_remove(&source->core_listener);
        pw_core_disconnect(source->core);
        source->core = NULL;
    }
    if (source->context != NULL) {
        pw_context_destroy(source->context);
        source->context = NULL;
    }
    if (source->loop_entered) {
        pw_loop_leave(source->loop);
        source->loop_entered = false;
    }
    if (source->main_loop != NULL) {
        pw_main_loop_destroy(source->main_loop);
        source->main_loop = NULL;
    }
    source->loop = NULL;
    source->sink_count = 0;
    source->default_sink[0] = '\0';
    source->reconnect_pending = false;
}

static void schedule_reconnect(struct i3sd_pipewire_source *source,
                               uint64_t now_ns) {
    publish_unavailable(source);
    close_native(source);
    source->retry_deadline_ns = now_ns + I3SD_PIPEWIRE_RETRY_NS;
}

static struct pipewire_sink *find_sink(struct i3sd_pipewire_source *source,
                                       const char *name) {
    for (size_t index = 0; index < source->sink_count; index++) {
        if (strcmp(source->sinks[index].name, name) == 0) {
            return &source->sinks[index];
        }
    }
    return NULL;
}

static void node_param(void *data, int seq, uint32_t id, uint32_t index,
                       uint32_t next, const struct spa_pod *param) {
    (void)seq;
    (void)index;
    (void)next;
    struct i3sd_pipewire_source *source = data;
    if (id != SPA_PARAM_Props || param == NULL) {
        return;
    }

    bool muted = false;
    bool soft_muted = false;
    float scalar_volume = -1.0F;
    uint32_t volume_size = 0;
    uint32_t volume_type = SPA_TYPE_None;
    uint32_t volume_count = 0;
    const float *volumes = NULL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    int result = spa_pod_parse_object(
        param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_volume,
        SPA_POD_OPT_Float(&scalar_volume), SPA_PROP_mute,
        SPA_POD_OPT_Bool(&muted), SPA_PROP_softMute,
        SPA_POD_OPT_Bool(&soft_muted), SPA_PROP_channelVolumes,
        SPA_POD_OPT_Array(&volume_size, &volume_type, &volume_count, &volumes));
#pragma GCC diagnostic pop
    if (result < 0 ||
        (volume_count > 0 &&
         (volume_size != sizeof(float) || volume_type != SPA_TYPE_Float)) ||
        (volume_count == 0 && scalar_volume < 0.0F)) {
        return;
    }

    /* Per-channel values carry the effective sink volume when available. */
    double effective =
        volume_count > 0 ? 0.0 : (scalar_volume < 0.0F ? 0.0 : scalar_volume);
    for (uint32_t channel = 0; channel < volume_count; channel++) {
        if (volumes[channel] > effective) {
            effective = volumes[channel];
        }
    }
    /* WirePlumber exposes the user-facing level as the cubic-root scale. */
    publish_volume(source, cbrt(effective), muted || soft_muted);
}

static const struct pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .param = node_param,
};

static void bind_default_node(struct i3sd_pipewire_source *source) {
    if (source->node != NULL || source->default_sink[0] == '\0') {
        return;
    }
    struct pipewire_sink *sink = find_sink(source, source->default_sink);
    if (sink == NULL) {
        return;
    }
    const uint32_t bind_version = sink->version < (uint32_t)PW_VERSION_NODE
                                      ? sink->version
                                      : (uint32_t)PW_VERSION_NODE;
    source->node = pw_registry_bind(source->registry, sink->id,
                                    PW_TYPE_INTERFACE_Node, bind_version, 0);
    if (source->node == NULL) {
        return;
    }
    source->node_id = sink->id;
    pw_node_add_listener(source->node, &source->node_listener, &node_events,
                         source);
    uint32_t parameters[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(source->node, parameters,
                             SPA_N_ELEMENTS(parameters));
    pw_node_enum_params(source->node, 0, SPA_PARAM_Props, 0, UINT32_MAX, NULL);
}

static int metadata_property(void *data, uint32_t subject, const char *key,
                             const char *type, const char *value) {
    (void)subject;
    (void)type;
    struct i3sd_pipewire_source *source = data;
    if (key == NULL || strcmp(key, "default.audio.sink") != 0) {
        return 0;
    }

    char parsed[I3SD_PIPEWIRE_NAME_LIMIT + 1] = {0};
    if (value != NULL) {
        yyjson_doc *document = yyjson_read(value, strlen(value), 0);
        yyjson_val *root =
            document == NULL ? NULL : yyjson_doc_get_root(document);
        yyjson_val *name =
            yyjson_is_obj(root) ? yyjson_obj_get(root, "name") : NULL;
        const char *text = yyjson_is_str(name) ? yyjson_get_str(name) : NULL;
        if (text != NULL && strlen(text) <= I3SD_PIPEWIRE_NAME_LIMIT) {
            strcpy(parsed, text);
        }
        yyjson_doc_free(document);
    }
    if (strcmp(source->default_sink, parsed) == 0) {
        return 0;
    }
    detach_node(source);
    publish_unavailable(source);
    strcpy(source->default_sink, parsed);
    bind_default_node(source);
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

static void registry_global(void *data, uint32_t id, uint32_t permissions,
                            const char *type, uint32_t version,
                            const struct spa_dict *props) {
    (void)permissions;
    struct i3sd_pipewire_source *source = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
        source->metadata == NULL && props != NULL) {
        const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name != NULL && strcmp(name, "default") == 0) {
            const uint32_t bind_version =
                version < (uint32_t)PW_VERSION_METADATA
                    ? version
                    : (uint32_t)PW_VERSION_METADATA;
            source->metadata =
                pw_registry_bind(source->registry, id,
                                 PW_TYPE_INTERFACE_Metadata, bind_version, 0);
            if (source->metadata != NULL) {
                source->metadata_id = id;
                pw_metadata_add_listener(source->metadata,
                                         &source->metadata_listener,
                                         &metadata_events, source);
            }
        }
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == NULL ||
        source->sink_count == I3SD_PIPEWIRE_SINK_LIMIT) {
        return;
    }
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (media_class == NULL || strcmp(media_class, "Audio/Sink") != 0 ||
        name == NULL || strlen(name) > I3SD_PIPEWIRE_NAME_LIMIT) {
        return;
    }
    struct pipewire_sink *sink = &source->sinks[source->sink_count++];
    sink->id = id;
    sink->version = version;
    strcpy(sink->name, name);
    bind_default_node(source);
}

static void registry_global_remove(void *data, uint32_t id) {
    struct i3sd_pipewire_source *source = data;
    if (source->node != NULL && source->node_id == id) {
        detach_node(source);
        publish_unavailable(source);
    }
    if (source->metadata != NULL && source->metadata_id == id) {
        detach_metadata(source);
        source->default_sink[0] = '\0';
        publish_unavailable(source);
    }
    for (size_t index = 0; index < source->sink_count; index++) {
        if (source->sinks[index].id != id) {
            continue;
        }
        source->sinks[index] = source->sinks[source->sink_count - 1];
        source->sink_count--;
        break;
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void core_error(void *data, uint32_t id, int seq, int result,
                       const char *message) {
    (void)seq;
    (void)message;
    struct i3sd_pipewire_source *source = data;
    if (id == PW_ID_CORE || result == -EPIPE) {
        /* Teardown happens after pw_loop_iterate returns. */
        source->reconnect_pending = true;
    }
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_error,
};

static bool open_native(struct i3sd_pipewire_source *source, uint64_t now_ns) {
    source->main_loop = pw_main_loop_new(NULL);
    if (source->main_loop == NULL) {
        source->retry_deadline_ns = now_ns + I3SD_PIPEWIRE_RETRY_NS;
        return false;
    }
    source->loop = pw_main_loop_get_loop(source->main_loop);
    pw_loop_enter(source->loop);
    source->loop_entered = true;
    source->context = pw_context_new(source->loop, NULL, 0);
    if (source->context == NULL) {
        schedule_reconnect(source, now_ns);
        return false;
    }
    source->core = pw_context_connect(source->context, NULL, 0);
    if (source->core == NULL) {
        schedule_reconnect(source, now_ns);
        return false;
    }
    pw_core_add_listener(source->core, &source->core_listener, &core_events,
                         source);
    source->registry =
        pw_core_get_registry(source->core, PW_VERSION_REGISTRY, 0);
    if (source->registry == NULL) {
        schedule_reconnect(source, now_ns);
        return false;
    }
    pw_registry_add_listener(source->registry, &source->registry_listener,
                             &registry_events, source);

    const int fd = pw_loop_get_fd(source->loop);
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLERR | EPOLLHUP,
        .data.u64 = SOURCE_PIPEWIRE,
    };
    if (fd < 0 ||
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        schedule_reconnect(source, now_ns);
        return false;
    }
    source->registered_fd = fd;
    source->retry_deadline_ns = 0;
    return true;
}

struct i3sd_pipewire_source *i3sd_pipewire_create(struct app *app) {
    struct i3sd_pipewire_source *source = calloc(1, sizeof(*source));
    if (source == NULL) {
        return NULL;
    }
    source->app = app;
    source->registered_fd = -1;
    source->metadata_id = SPA_ID_INVALID;
    source->node_id = SPA_ID_INVALID;
    source->state_revision = 1;
    pw_init(NULL, NULL);
    return source;
}

static void free_subscription(struct pipewire_subscription *entry) {
    luaL_unref(entry->block->generation->lua, LUA_REGISTRYINDEX,
               entry->callback_ref);
    free(entry);
}

void i3sd_pipewire_destroy(struct i3sd_pipewire_source *source) {
    if (source == NULL) {
        return;
    }
    close_native(source);
    struct pipewire_subscription *entry = source->subscriptions;
    while (entry != NULL) {
        struct pipewire_subscription *next = entry->next;
        free_subscription(entry);
        entry = next;
    }
    pw_deinit();
    free(source);
}

static void cancel_id(struct i3sd_pipewire_source *source, uint64_t id) {
    struct pipewire_subscription **cursor = &source->subscriptions;
    while (*cursor != NULL) {
        struct pipewire_subscription *entry = *cursor;
        if (entry->id == id) {
            *cursor = entry->next;
            free_subscription(entry);
            return;
        }
        cursor = &entry->next;
    }
}

static int lua_handle_cancel(lua_State *lua) {
    struct lua_pipewire_handle *handle =
        luaL_checkudata(lua, 1, handle_metatable);
    if (handle->source != NULL && handle->subscription_id != 0) {
        cancel_id(handle->source, handle->subscription_id);
        handle->subscription_id = 0;
    }
    return 0;
}

void i3sd_pipewire_register_lua(lua_State *lua) {
    luaL_newmetatable(lua, handle_metatable);
    lua_newtable(lua);
    lua_pushcfunction(lua, lua_handle_cancel);
    lua_setfield(lua, -2, "cancel");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, lua_handle_cancel);
    lua_setfield(lua, -2, "__gc");
    lua_pop(lua, 1);
}

int i3sd_pipewire_watch(lua_State *lua, struct i3sd_pipewire_source *source,
                        struct block *block, int callback_index) {
    luaL_checktype(lua, callback_index, LUA_TFUNCTION);
    struct pipewire_subscription *entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return luaL_error(lua, "out of memory creating PipeWire subscription");
    }
    uint64_t id = ++source->next_subscription_id;
    if (id == 0) {
        id = ++source->next_subscription_id;
    }
    entry->block = block;
    entry->id = id;
    lua_pushvalue(lua, callback_index);
    entry->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    entry->next = source->subscriptions;
    source->subscriptions = entry;

    struct lua_pipewire_handle *handle = lua_newuserdata(lua, sizeof(*handle));
    handle->source = source;
    handle->subscription_id = id;
    luaL_getmetatable(lua, handle_metatable);
    lua_setmetatable(lua, -2);
    return 1;
}

void i3sd_pipewire_cancel_owner(struct i3sd_pipewire_source *source,
                                struct block *block) {
    if (source == NULL) {
        return;
    }
    struct pipewire_subscription **cursor = &source->subscriptions;
    while (*cursor != NULL) {
        struct pipewire_subscription *entry = *cursor;
        if (entry->block == block) {
            *cursor = entry->next;
            free_subscription(entry);
            continue;
        }
        cursor = &entry->next;
    }
}

void i3sd_pipewire_retire_generation(struct i3sd_pipewire_source *source,
                                     struct generation *generation) {
    if (source == NULL) {
        return;
    }
    struct pipewire_subscription **cursor = &source->subscriptions;
    while (*cursor != NULL) {
        struct pipewire_subscription *entry = *cursor;
        if (entry->block->generation == generation) {
            *cursor = entry->next;
            free_subscription(entry);
            continue;
        }
        cursor = &entry->next;
    }
}

void i3sd_pipewire_reconcile(struct i3sd_pipewire_source *source,
                             uint64_t now_ns) {
    if (source == NULL) {
        return;
    }
    if (!source_needed(source)) {
        if (source->main_loop != NULL) {
            close_native(source);
        }
        source->retry_deadline_ns = 0;
        return;
    }
    if (source->main_loop == NULL && source->retry_deadline_ns <= now_ns) {
        open_native(source, now_ns);
    }
    i3sd_pipewire_notify(source);
}

void i3sd_pipewire_process(struct i3sd_pipewire_source *source,
                           uint64_t now_ns) {
    if (source == NULL || source->loop == NULL) {
        return;
    }
    for (size_t count = 0; count < I3SD_PIPEWIRE_DISPATCH_BUDGET; count++) {
        int result = pw_loop_iterate(source->loop, 0);
        if (result > 0) {
            continue;
        }
        if (result < 0) {
            source->reconnect_pending = true;
        }
        break;
    }
    if (source->reconnect_pending) {
        schedule_reconnect(source, now_ns);
    }
    i3sd_pipewire_reconcile(source, now_ns);
}

uint64_t i3sd_pipewire_deadline(const struct i3sd_pipewire_source *source) {
    if (source == NULL || !source_needed(source) || source->main_loop != NULL ||
        source->retry_deadline_ns == 0) {
        return UINT64_MAX;
    }
    return source->retry_deadline_ns;
}

#else

struct i3sd_pipewire_source {
    struct app *app;
};

struct i3sd_pipewire_source *i3sd_pipewire_create(struct app *app) {
    struct i3sd_pipewire_source *source = calloc(1, sizeof(*source));
    if (source != NULL) {
        source->app = app;
    }
    return source;
}

void i3sd_pipewire_destroy(struct i3sd_pipewire_source *source) {
    free(source);
}

void i3sd_pipewire_register_lua(lua_State *lua) { (void)lua; }

int i3sd_pipewire_watch(lua_State *lua, struct i3sd_pipewire_source *source,
                        struct block *block, int callback_index) {
    (void)source;
    (void)block;
    (void)callback_index;
    return luaL_error(lua, "i3sd was built without PipeWire support");
}

void i3sd_pipewire_cancel_owner(struct i3sd_pipewire_source *source,
                                struct block *block) {
    (void)source;
    (void)block;
}

void i3sd_pipewire_retire_generation(struct i3sd_pipewire_source *source,
                                     struct generation *generation) {
    (void)source;
    (void)generation;
}

void i3sd_pipewire_notify(struct i3sd_pipewire_source *source) { (void)source; }

void i3sd_pipewire_reconcile(struct i3sd_pipewire_source *source,
                             uint64_t now_ns) {
    (void)source;
    (void)now_ns;
}

void i3sd_pipewire_process(struct i3sd_pipewire_source *source,
                           uint64_t now_ns) {
    (void)source;
    (void)now_ns;
}

uint64_t i3sd_pipewire_deadline(const struct i3sd_pipewire_source *source) {
    (void)source;
    return UINT64_MAX;
}

#endif
