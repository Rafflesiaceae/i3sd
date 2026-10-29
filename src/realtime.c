#define _GNU_SOURCE

#include "realtime.h"

#include <lauxlib.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Realtime callbacks dispatched per turn, like monotonic timers. */
#define I3SD_REALTIME_BUDGET 256U
/* Armed while only observers exist, so discontinuities stay observable
 * without a periodic wakeup. It is re-armed if it ever expires. */
#define I3SD_CLOCK_SENTINEL UINT64_MAX
#define I3SD_CLOCK_SENTINEL_DELAY_NS (365ULL * 24 * 3600 * 1000000000ULL)

uint64_t i3sd_realtime_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || now.tv_sec < 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool observers_active(const struct generation *generation) {
    if (generation == NULL) {
        return false;
    }
    for (const struct clock_observer *observer = generation->clock_observers;
         observer != NULL; observer = observer->next) {
        if (!observer->cancelled && !observer->block->faulted) {
            return true;
        }
    }
    return false;
}

void i3sd_realtime_close(struct app *app) {
    if (app->clock_fd >= 0) {
        epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, app->clock_fd, NULL);
        close(app->clock_fd);
    }
    app->clock_fd = -1;
    app->clock_armed_ns = 0;
}

void i3sd_realtime_reconcile(struct app *app) {
    const struct i3sd_timer *earliest = i3sd_timer_peek(&app->realtime_heap);
    if (earliest == NULL && !observers_active(app->current)) {
        i3sd_realtime_close(app);
        return;
    }
    if (app->clock_fd < 0) {
        const int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        struct epoll_event event = {.events = EPOLLIN,
                                    .data.u64 = SOURCE_CLOCK};
        if (fd < 0 ||
            epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
            /* Without the timerfd realtime timers cannot fire; report it
             * once per attempt and retry on the next loop turn. */
            fprintf(stderr, "i3sd: unable to create realtime timer: %s\n",
                    strerror(errno));
            if (fd >= 0) {
                close(fd);
            }
            return;
        }
        app->clock_fd = fd;
        app->clock_armed_ns = 0;
    }
    uint64_t wanted =
        earliest != NULL ? earliest->deadline_ns : I3SD_CLOCK_SENTINEL;
    if (wanted == 0) {
        /* A zero it_value would disarm the timer instead of firing it, and
         * 0 marks clock_armed_ns as disarmed. */
        wanted = 1;
    }
    if (wanted == app->clock_armed_ns) {
        return;
    }
    const uint64_t deadline_ns =
        wanted == I3SD_CLOCK_SENTINEL
            ? i3sd_realtime_now_ns() + I3SD_CLOCK_SENTINEL_DELAY_NS
            : wanted;
    const struct itimerspec specification = {
        .it_value =
            {
                .tv_sec = (time_t)(deadline_ns / 1000000000ULL),
                .tv_nsec = (long)(deadline_ns % 1000000000ULL),
            },
    };
    if (timerfd_settime(app->clock_fd,
                        TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                        &specification, NULL) < 0) {
        if (errno != ECANCELED) {
            fprintf(stderr, "i3sd: unable to arm realtime timer: %s\n",
                    strerror(errno));
            return;
        }
        /* Linux reports an undrained discontinuity here but still arms the
         * timer with the new value. */
        app->clock_changed = true;
    }
    app->clock_armed_ns = wanted;
}

static void notify_clock_change(struct app *app) {
    struct generation *generation = app->current;
    if (generation == NULL) {
        return;
    }
    struct clock_observer *observer = generation->clock_observers;
    while (observer != NULL) {
        /* A failing callback faults its block but never unlinks observers. */
        struct clock_observer *next = observer->next;
        struct block *block = observer->block;
        if (!observer->cancelled && !block->faulted) {
            lua_State *lua = generation->lua;
            lua_rawgeti(lua, LUA_REGISTRYINDEX, observer->callback_ref);
            lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
            if (lua_pcall(lua, 1, 0, 0) != 0) {
                i3sd_log_lua_error(block, "clock change");
                i3sd_fault_block(block);
            }
        }
        observer = next;
    }
}

static void dispatch_due(struct app *app) {
    const uint64_t now_ns = i3sd_realtime_now_ns();
    for (size_t count = 0; count < I3SD_REALTIME_BUDGET; count++) {
        struct i3sd_timer *native =
            i3sd_timer_pop_due(&app->realtime_heap, now_ns);
        if (native == NULL) {
            return;
        }
        struct logical_timer *timer = native->userdata;
        struct block *block = timer->block;
        if (timer->cancelled || block->generation != app->current ||
            block->faulted) {
            continue;
        }
        lua_State *lua = block->generation->lua;
        lua_rawgeti(lua, LUA_REGISTRYINDEX, timer->callback_ref);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, block->context_ref);
        if (lua_pcall(lua, 1, 0, 0) != 0) {
            i3sd_log_lua_error(block, "realtime timer");
            i3sd_fault_block(block);
        }
    }
}

void i3sd_realtime_process(struct app *app) {
    if (app->clock_fd >= 0) {
        uint64_t expirations;
        const ssize_t count =
            read(app->clock_fd, &expirations, sizeof(expirations));
        if (count < 0 && errno == ECANCELED) {
            app->clock_changed = true;
        }
        if (count >= 0 || errno == ECANCELED) {
            /* Expiry and cancellation both leave the timer disarmed. */
            app->clock_armed_ns = 0;
        }
    }
    /* Observers run first so they may cancel or replace realtime timers
     * before deadlines that became due through the jump are dispatched. */
    if (app->clock_changed) {
        app->clock_changed = false;
        notify_clock_change(app);
    }
    dispatch_due(app);
}
