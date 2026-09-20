#define _GNU_SOURCE

#include "power_supply_events.h"

#include <lauxlib.h>

#include <errno.h>
#include <linux/netlink.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define I3SD_POWER_SUPPLY_RETRY_NS 5000000000ULL
#define I3SD_UEVENT_BUFFER_SIZE 8192U
#define I3SD_UEVENT_DISPATCH_BUDGET 256U

static bool source_needed(const struct app *app) {
    if (app->current == NULL) {
        return false;
    }
    for (const struct power_supply_subscription *subscription =
             app->current->power_supply_subscriptions;
         subscription != NULL; subscription = subscription->next) {
        if (!subscription->cancelled && !subscription->block->faulted) {
            return true;
        }
    }
    return false;
}

static bool event_is_power_supply(const char *event, size_t event_size) {
    static const char subsystem[] = "SUBSYSTEM=power_supply";
    size_t offset = 0;
    while (offset < event_size) {
        const char *field = event + offset;
        const char *end = memchr(field, '\0', event_size - offset);
        const size_t field_size =
            end == NULL ? event_size - offset : (size_t)(end - field);
        if (field_size == sizeof(subsystem) - 1 &&
            memcmp(field, subsystem, sizeof(subsystem) - 1) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        offset += field_size + 1;
    }
    return false;
}

void i3sd_power_supply_notify(struct app *app) {
    struct generation *generation = app->current;
    if (generation == NULL || generation->staging) {
        return;
    }
    struct power_supply_subscription *subscription =
        generation->power_supply_subscriptions;
    while (subscription != NULL) {
        struct power_supply_subscription *next = subscription->next;
        struct block *block = subscription->block;
        if (!subscription->cancelled && !block->faulted) {
            lua_State *lua = generation->lua;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, subscription->callback_ref);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
            if (lua_pcall(lua, 1, 0, 0) != 0) {
                i3sd_log_lua_error(block, "power supply event");
                i3sd_fault_block(block);
            }
        }
        subscription = next;
    }
}

void i3sd_power_supply_close(struct power_supply_source *source) {
    if (source->fd >= 0) {
        epoll_ctl(source->app->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);
        close(source->fd);
    }
    source->fd = -1;
}

static bool open_source(struct power_supply_source *source, uint64_t now_ns) {
    /* Kernel power_supply changes are broadcast on the kobject uevent group. */
    const int fd = socket(AF_NETLINK,
                          SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        source->retry_deadline_ns = now_ns + I3SD_POWER_SUPPLY_RETRY_NS;
        return false;
    }
    struct sockaddr_nl address = {
        .nl_family = AF_NETLINK,
        .nl_groups = 1,
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        source->retry_deadline_ns = now_ns + I3SD_POWER_SUPPLY_RETRY_NS;
        return false;
    }
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLERR | EPOLLHUP,
        .data.u64 = SOURCE_POWER_SUPPLY,
    };
    if (epoll_ctl(source->app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        close(fd);
        source->retry_deadline_ns = now_ns + I3SD_POWER_SUPPLY_RETRY_NS;
        return false;
    }
    source->fd = fd;
    source->retry_deadline_ns = 0;
    return true;
}

void i3sd_power_supply_reconcile(struct power_supply_source *source,
                                 uint64_t now_ns) {
    if (!source_needed(source->app)) {
        if (source->fd >= 0) {
            i3sd_power_supply_close(source);
        }
        source->retry_deadline_ns = 0;
        return;
    }
    if (source->fd < 0 && source->retry_deadline_ns <= now_ns) {
        open_source(source, now_ns);
    }
}

void i3sd_power_supply_process(struct power_supply_source *source,
                              uint64_t now_ns) {
    if (source->fd < 0) {
        return;
    }
    bool changed = false;
    /* Coalesce a burst of kernel notifications into one fresh sysfs snapshot. */
    for (size_t count = 0; count < I3SD_UEVENT_DISPATCH_BUDGET; count++) {
        char event[I3SD_UEVENT_BUFFER_SIZE];
        struct sockaddr_nl sender = {0};
        socklen_t sender_size = sizeof(sender);
        const ssize_t received = recvfrom(
            source->fd, event, sizeof(event), MSG_DONTWAIT | MSG_TRUNC,
            (struct sockaddr *)&sender, &sender_size);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            i3sd_power_supply_close(source);
            source->retry_deadline_ns = now_ns + I3SD_POWER_SUPPLY_RETRY_NS;
            return;
        }
        if ((size_t)received <= sizeof(event) && sender.nl_pid == 0 &&
            event_is_power_supply(event, (size_t)received)) {
            changed = true;
        }
    }
    if (changed) {
        i3sd_power_supply_notify(source->app);
    }
    i3sd_power_supply_reconcile(source, now_ns);
}

uint64_t i3sd_power_supply_deadline(const struct power_supply_source *source) {
    if (!source_needed(source->app) || source->fd >= 0 ||
        source->retry_deadline_ns == 0) {
        return UINT64_MAX;
    }
    return source->retry_deadline_ns;
}
