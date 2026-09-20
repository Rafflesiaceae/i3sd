#include "power_profiles.h"

#include <lauxlib.h>

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>

bool i3sd_power_profiles_needed(const struct app *app) {
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

void i3sd_power_profiles_notify(struct app *app) {
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
            i3sd_log_lua_error(block, "power profile event");
            i3sd_fault_block(block);
        }
    }
}

static int active_reply(sd_bus_message *message, void *userdata,
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
        i3sd_power_profiles_notify(source->app);
    }
    return 0;
}

static int list_reply(sd_bus_message *message, void *userdata,
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
        i3sd_power_profiles_notify(source->app);
    }
    return 0;
}

static void request_snapshots(struct power_profiles_source *source) {
    if (source->bus == NULL) {
        return;
    }
    if (!source->active_query_inflight) {
        int result = sd_bus_call_method_async(
            source->bus, NULL, "org.freedesktop.UPower.PowerProfiles",
            "/org/freedesktop/UPower/PowerProfiles",
            "org.freedesktop.DBus.Properties", "Get", active_reply, source,
            "ss", "org.freedesktop.UPower.PowerProfiles", "ActiveProfile");
        source->active_query_inflight = result >= 0;
    }
    if (!source->profiles_query_inflight) {
        int result = sd_bus_call_method_async(
            source->bus, NULL, "org.freedesktop.UPower.PowerProfiles",
            "/org/freedesktop/UPower/PowerProfiles",
            "org.freedesktop.DBus.Properties", "Get", list_reply, source, "ss",
            "org.freedesktop.UPower.PowerProfiles", "Profiles");
        source->profiles_query_inflight = result >= 0;
    }
}

static int properties_changed(sd_bus_message *message, void *userdata,
                              sd_bus_error *ret_error) {
    (void)message;
    (void)ret_error;
    request_snapshots(userdata);
    return 0;
}

static int match_installed(sd_bus_message *message, void *userdata,
                           sd_bus_error *ret_error) {
    (void)ret_error;
    if (!sd_bus_message_is_method_error(message, NULL)) {
        request_snapshots(userdata);
    }
    return 0;
}

void i3sd_power_profiles_close(struct power_profiles_source *source) {
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

static bool open_source(struct power_profiles_source *source, uint64_t now_ns) {
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
    result =
        sd_bus_add_match_async(source->bus, &source->match_slot, match,
                               properties_changed, match_installed, source);
    if (result < 0) {
        i3sd_power_profiles_close(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return false;
    }
    source->retry_deadline_ns = 0;
    return true;
}

void i3sd_power_profiles_reconcile(struct power_profiles_source *source,
                                   uint64_t now_ns) {
    if (!i3sd_power_profiles_needed(source->app)) {
        if (source->bus != NULL) {
            i3sd_power_profiles_close(source);
        }
        return;
    }
    if (source->bus == NULL) {
        if (source->retry_deadline_ns > now_ns ||
            !open_source(source, now_ns)) {
            return;
        }
    }
    const int fd = sd_bus_get_fd(source->bus);
    const int poll_events = sd_bus_get_events(source->bus);
    if (fd < 0 || poll_events < 0) {
        i3sd_power_profiles_close(source);
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
        i3sd_power_profiles_close(source);
        source->retry_deadline_ns = now_ns + 5000000000ULL;
        return;
    }
    source->registered_fd = fd;
    source->registered_events = events;
}

void i3sd_power_profiles_process(struct power_profiles_source *source,
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
            i3sd_power_profiles_close(source);
            source->retry_deadline_ns = now_ns + 5000000000ULL;
        }
        break;
    }
    i3sd_power_profiles_reconcile(source, now_ns);
}

static int set_reply(sd_bus_message *message, void *userdata,
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

bool i3sd_power_profile_set(struct app *app, const char *profile) {
    struct power_profiles_source *source = &app->power_profiles;
    if (source->bus == NULL) {
        return false;
    }
    return sd_bus_call_method_async(
               source->bus, NULL, "org.freedesktop.UPower.PowerProfiles",
               "/org/freedesktop/UPower/PowerProfiles",
               "org.freedesktop.DBus.Properties", "Set", set_reply, source,
               "ssv", "org.freedesktop.UPower.PowerProfiles", "ActiveProfile",
               "s", profile) >= 0;
}

bool i3sd_power_profile_known(const struct power_profiles_source *source,
                              const char *profile) {
    for (size_t index = 0; index < source->profile_count; index++) {
        if (strcmp(source->profiles[index], profile) == 0) {
            return true;
        }
    }
    return false;
}
