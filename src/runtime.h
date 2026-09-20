#ifndef I3SD_RUNTIME_H
#define I3SD_RUNTIME_H

/* Internal state shared by the reactor and its native event sources. */

#include "dbus.h"
#include "i3sd/buffer.h"
#include "i3sd/click.h"
#include "i3sd/output.h"
#include "i3sd/timer.h"

#include <lua.h>
#include <systemd/sd-bus.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define I3SD_MAX_BLOCKS 256U
#define I3SD_MAX_TIMERS 4096U
#define I3SD_POWER_PROFILE_LIMIT 16U
#define I3SD_POWER_PROFILE_NAME_LIMIT 63U

enum source_cookie {
    SOURCE_SIGNAL = 1,
    SOURCE_STDIN = 2,
    SOURCE_STDOUT = 3,
    SOURCE_INOTIFY = 4,
    SOURCE_SYSTEM_BUS = 5,
    SOURCE_USER_BUS = 6,
    SOURCE_POWER_PROFILES = 7,
    SOURCE_SPAWN = 8,
    SOURCE_PIPEWIRE = 9,
    SOURCE_FIXED_MAX = SOURCE_PIPEWIRE,
};

struct app;
struct generation;
struct block;
struct i3sd_pipewire_source;

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

struct spawned_process {
    pid_t pid;
    int output_fd;
    struct block *block;
    int callback_ref;
    uint64_t serial;
    size_t output_limit;
    int wait_status;
    struct i3sd_buffer output;
    bool active;
    bool output_closed;
    bool exited;
    bool overflow;
    bool io_error;
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
    struct i3sd_dbus_generation *dbus;
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
    struct i3sd_pipewire_source *pipewire;
    struct i3sd_dbus_runtime *dbus;
    struct spawned_process spawn;
    uint64_t next_spawn_serial;
    struct i3sd_buffer frame;
    struct identity identities[4096];
    size_t identity_count;
    uint64_t next_registration_cookie;
    size_t prelude_offset;
    uint64_t last_render_ns;
    bool stdout_registered;
    bool render_dirty;
    bool reload_dirty;
    bool running;
    bool debug;
};

/* Event sources use these callbacks to preserve block fault isolation. */
void i3sd_fault_block(struct block *block);
void i3sd_log_lua_error(struct block *block, const char *phase);

#endif
