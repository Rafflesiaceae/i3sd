#include "systemd.h"

#include <lauxlib.h>

#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>

static bool scope_uses(enum systemd_scope subscription_scope,
                       enum systemd_scope bus_scope) {
    return subscription_scope == SYSTEMD_SCOPE_BOTH ||
           subscription_scope == bus_scope;
}

bool i3sd_systemd_bus_needed(const struct app *app, enum systemd_scope scope) {
    if (app->current == NULL) {
        return false;
    }
    for (struct systemd_subscription *subscription =
             app->current->systemd_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (!subscription->cancelled &&
            scope_uses(subscription->scope, scope)) {
            return true;
        }
    }
    return false;
}

void i3sd_systemd_notify(struct app *app) {
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
        if ((scope_uses(subscription->scope, SYSTEMD_SCOPE_SYSTEM) &&
             !system_bus->count_valid) ||
            (scope_uses(subscription->scope, SYSTEMD_SCOPE_USER) &&
             !user_bus->count_valid)) {
            continue;
        }

        uint32_t total = 0;
        lua_State *lua = generation->lua;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, subscription->callback_ref);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
        lua_newtable(lua);
        if (scope_uses(subscription->scope, SYSTEMD_SCOPE_SYSTEM)) {
            total += system_bus->failed_count;
            lua_pushinteger(lua, system_bus->failed_count);
            lua_setfield(lua, -2, "system_count");
        }
        if (scope_uses(subscription->scope, SYSTEMD_SCOPE_USER)) {
            total += user_bus->failed_count;
            lua_pushinteger(lua, user_bus->failed_count);
            lua_setfield(lua, -2, "user_count");
        }
        lua_pushinteger(lua, total);
        lua_setfield(lua, -2, "count");
        if (lua_pcall(lua, 2, 0, 0) != 0) {
            i3sd_log_lua_error(block, "systemd event");
            i3sd_fault_block(block);
        }
    }
}

static int snapshot_reply(sd_bus_message *message, void *userdata,
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
        i3sd_systemd_notify(source->app);
    }
    return 0;
}

static void request_snapshot(struct systemd_bus *source) {
    if (source->bus == NULL || source->query_inflight) {
        return;
    }
    int result = sd_bus_call_method_async(
        source->bus, NULL, "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1", "org.freedesktop.DBus.Properties", "Get",
        snapshot_reply, source, "ss", "org.freedesktop.systemd1.Manager",
        "NFailedUnits");
    if (result >= 0) {
        source->query_inflight = true;
    }
}

static int property_changed(sd_bus_message *message, void *userdata,
                            sd_bus_error *ret_error) {
    (void)message;
    (void)ret_error;
    request_snapshot(userdata);
    return 0;
}

static int subscribe_reply(sd_bus_message *message, void *userdata,
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
    request_snapshot(source);
    return 0;
}

static void request_subscribe(struct systemd_bus *source) {
    if (source->bus == NULL || source->subscribe_inflight) {
        return;
    }
    int result = sd_bus_call_method_async(
        source->bus, NULL, "org.freedesktop.systemd1",
        "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager",
        "Subscribe", subscribe_reply, source, NULL);
    if (result >= 0) {
        source->subscribe_inflight = true;
    }
}

static int match_installed(sd_bus_message *message, void *userdata,
                           sd_bus_error *ret_error) {
    (void)ret_error;
    if (!sd_bus_message_is_method_error(message, NULL)) {
        request_subscribe(userdata);
    }
    return 0;
}

void i3sd_systemd_close(struct systemd_bus *source) {
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

static bool open_bus(struct systemd_bus *source, uint64_t now_ns) {
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
                                    property_changed, match_installed, source);
    if (result < 0) {
        i3sd_systemd_close(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->retry_deadline_ns = 0;
    return true;
}

bool i3sd_systemd_reconcile(struct systemd_bus *source, uint64_t now_ns) {
    if (!i3sd_systemd_bus_needed(source->app, source->scope)) {
        if (source->bus != NULL) {
            i3sd_systemd_close(source);
        }
        return true;
    }
    if (source->bus == NULL) {
        if (source->retry_deadline_ns > now_ns) {
            return true;
        }
        if (!open_bus(source, now_ns)) {
            return true;
        }
    }

    const int fd = sd_bus_get_fd(source->bus);
    const int poll_events = sd_bus_get_events(source->bus);
    if (fd < 0 || poll_events < 0) {
        i3sd_systemd_close(source);
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
        i3sd_systemd_close(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->registered_fd = fd;
    source->registered_events = events;
    return true;
}

void i3sd_systemd_process(struct systemd_bus *source, uint64_t now_ns) {
    if (source->bus == NULL) {
        return;
    }
    for (size_t count = 0; count < 256; count++) {
        int result = sd_bus_process(source->bus, NULL);
        if (result > 0) {
            continue;
        }
        if (result < 0) {
            i3sd_systemd_close(source);
            source->retry_deadline_ns = now_ns + 5000000000ULL;
        }
        break;
    }
    i3sd_systemd_reconcile(source, now_ns);
}
