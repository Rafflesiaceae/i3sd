#define _GNU_SOURCE

#include "collectors.h"
#include "i3sd/utf8.h"
#include "nvidia.h"

#include <lauxlib.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

/* Collectors are synchronous bounded snapshots; the reactor calls them only
 * from an explicit Lua update callback. */
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
        if (lua_type(lua, -2) != LUA_TSTRING) {
            luaL_error(lua, "option keys must be strings");
        }
        const char *key = lua_tostring(lua, -2);
        if (!strict_known_key(key, known, known_count)) {
            luaL_error(lua, "unknown option '%s'", key);
        }
        lua_pop(lua, 1);
    }
}

static bool valid_text(const char *text, size_t len, size_t limit) {
    return len <= limit && memchr(text, '\0', len) == NULL &&
           i3sd_valid_utf8(text, len);
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

static void push_snapshot_header(lua_State *lua,
                                 const struct i3sd_collector_host *host,
                                 const char *source_id) {
    lua_newtable(lua);
    host->push_uint64(lua, host->now_ns());
    lua_setfield(lua, -2, "timestamp_ns");
    lua_pushstring(lua, source_id);
    lua_setfield(lua, -2, "source_id");
    host->push_uint64(lua, 1);
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

static int sample_cpu(lua_State *lua, int options,
                      const struct i3sd_collector_host *host) {
    static const char *const fields[] = {"per_cpu"};
    check_strict_table(lua, options, fields, 1);
    const bool per_cpu = option_boolean(lua, options, "per_cpu", false);
    FILE *file = fopen("/proc/stat", "re");
    if (file == NULL) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "cpu", errno);
        return 2;
    }
    push_snapshot_header(lua, host, "proc-stat");
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
            host->push_uint64(lua, values[index]);
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

static int sample_load(lua_State *lua, int options,
                       const struct i3sd_collector_host *host) {
    check_strict_table(lua, options, NULL, 0);
    struct sysinfo information;
    if (sysinfo(&information) < 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "load", errno);
        return 2;
    }
    push_snapshot_header(lua, host, "sysinfo-load");
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

static int sample_memory(lua_State *lua, int options,
                         const struct i3sd_collector_host *host) {
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
    push_snapshot_header(lua, host, "proc-meminfo");
    host->push_uint64(lua, total);
    lua_setfield(lua, -2, "mem_total");
    host->push_uint64(lua, available);
    lua_setfield(lua, -2, "mem_available");
    host->push_uint64(lua, swap_total);
    lua_setfield(lua, -2, "swap_total");
    host->push_uint64(lua, swap_free);
    lua_setfield(lua, -2, "swap_free");
    return 1;
}

static int sample_filesystem(lua_State *lua, int options,
                             const struct i3sd_collector_host *host) {
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
    push_snapshot_header(lua, host, path);
    lua_pushvalue(lua, -2);
    lua_setfield(lua, -2, "path");
    host->push_uint64(lua, (uint64_t)status.f_blocks * status.f_frsize);
    lua_setfield(lua, -2, "total_bytes");
    host->push_uint64(lua, (uint64_t)status.f_bfree * status.f_frsize);
    lua_setfield(lua, -2, "free_bytes");
    host->push_uint64(lua, (uint64_t)status.f_bavail * status.f_frsize);
    lua_setfield(lua, -2, "available_bytes");
    lua_pushboolean(lua, (status.f_flag & ST_RDONLY) != 0);
    lua_setfield(lua, -2, "readonly");
    lua_remove(lua, -2);
    return 1;
}

static int sample_time(lua_State *lua, int options,
                       const struct i3sd_collector_host *host) {
    check_strict_table(lua, options, NULL, 0);
    struct timespec realtime, monotonic, boottime;
    if (clock_gettime(CLOCK_REALTIME, &realtime) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0 ||
        clock_gettime(CLOCK_BOOTTIME, &boottime) < 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(errno), "time", errno);
        return 2;
    }
    push_snapshot_header(lua, host, "linux-clocks");
#define SET_CLOCK(name, value)                                                 \
    do {                                                                       \
        host->push_uint64(lua, (uint64_t)(value).tv_sec * 1000000000ULL +      \
                                   (uint64_t)(value).tv_nsec);                 \
        lua_setfield(lua, -2, (name));                                         \
    } while (0)
    SET_CLOCK("realtime_ns", realtime);
    SET_CLOCK("monotonic_ns", monotonic);
    SET_CLOCK("boottime_ns", boottime);
#undef SET_CLOCK
    return 1;
}

static int sample_psi(lua_State *lua, int options,
                      const struct i3sd_collector_host *host) {
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
    push_snapshot_header(lua, host, path);
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
        host->push_uint64(lua, total);
        lua_setfield(lua, -2, "total_us");
        lua_setfield(lua, -2, mode);
    }
    fclose(file);
    lua_remove(lua, -2);
    return 1;
}

static int sample_file_stat(lua_State *lua, int options,
                            const struct i3sd_collector_host *host) {
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
    push_snapshot_header(lua, host, path);
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
        host->push_uint64(lua, (uint64_t)status.st_size);
        lua_setfield(lua, -2, "size_bytes");
        host->push_uint64(lua, status.st_dev);
        lua_setfield(lua, -2, "device");
        host->push_uint64(lua, status.st_ino);
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
        host->push_int64(lua, mtime_ns);
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

static int compare_supply_names(const void *left, const void *right) {
    return strcmp(left, right);
}

static bool read_supply_text(const char *name, const char *field, char *value,
                             size_t value_size) {
    char path[PATH_MAX];
    const int path_length = snprintf(path, sizeof(path),
                                     "/sys/class/power_supply/%s/%s", name,
                                     field);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return false;
    }
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return false;
    }
    const bool read = fgets(value, (int)value_size, file) != NULL;
    fclose(file);
    if (!read) {
        return false;
    }
    value[strcspn(value, "\r\n")] = '\0';
    return valid_text(value, strlen(value), value_size - 1);
}

static bool read_supply_counter(const char *name, const char *field,
                                int64_t *value) {
    char text[64];
    if (!read_supply_text(name, field, text, sizeof(text))) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

static void set_optional_supply_counter(
    lua_State *lua, const char *name, const char *attribute,
    const char *field, const struct i3sd_collector_host *host) {
    int64_t value;
    if (read_supply_counter(name, attribute, &value)) {
        host->push_int64(lua, value);
        lua_setfield(lua, -2, field);
    }
}

static int sample_power_supply(lua_State *lua, int options,
                               const struct i3sd_collector_host *host) {
    check_strict_table(lua, options, NULL, 0);
    DIR *directory = opendir("/sys/class/power_supply");
    if (directory == NULL) {
        const int saved_errno = errno;
        lua_pushnil(lua);
        push_error(lua, saved_errno == ENOENT ? "unsupported" : "unavailable",
                   strerror(saved_errno), "power_supply", saved_errno);
        return 2;
    }

    /* Power-supply inventories are small; keep the snapshot bounded. */
    char names[256][NAME_MAX + 1];
    size_t name_count = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!valid_text(entry->d_name, strlen(entry->d_name), NAME_MAX)) {
            continue;
        }
        if (name_count == sizeof(names) / sizeof(names[0])) {
            closedir(directory);
            lua_pushnil(lua);
            push_error(lua, "too_large", "too many power supplies",
                       "power_supply", 0);
            return 2;
        }
        memcpy(names[name_count++], entry->d_name, strlen(entry->d_name) + 1);
    }
    const int read_errno = errno;
    closedir(directory);
    if (read_errno != 0) {
        lua_pushnil(lua);
        push_error(lua, "unavailable", strerror(read_errno), "power_supply",
                   read_errno);
        return 2;
    }
    qsort(names, name_count, sizeof(names[0]), compare_supply_names);

    push_snapshot_header(lua, host, "/sys/class/power_supply");
    const int snapshot = lua_gettop(lua);
    lua_newtable(lua);
    size_t supply_index = 1;
    for (size_t index = 0; index < name_count; index++) {
        char type[64];
        if (!read_supply_text(names[index], "type", type, sizeof(type))) {
            continue;
        }
        lua_newtable(lua);
        lua_pushstring(lua, names[index]);
        lua_setfield(lua, -2, "name");
        lua_pushstring(lua, type);
        lua_setfield(lua, -2, "type");

        char status[64];
        if (read_supply_text(names[index], "status", status, sizeof(status))) {
            lua_pushstring(lua, status);
            lua_setfield(lua, -2, "status");
        }
        set_optional_supply_counter(lua, names[index], "energy_now",
                                   "energy_now_uwh", host);
        set_optional_supply_counter(lua, names[index], "energy_full",
                                   "energy_full_uwh", host);
        set_optional_supply_counter(lua, names[index], "energy_full_design",
                                   "energy_full_design_uwh", host);
        set_optional_supply_counter(lua, names[index], "charge_now",
                                   "charge_now_uah", host);
        set_optional_supply_counter(lua, names[index], "charge_full",
                                   "charge_full_uah", host);
        set_optional_supply_counter(lua, names[index], "charge_full_design",
                                   "charge_full_design_uah", host);
        set_optional_supply_counter(lua, names[index], "power_now",
                                   "power_now_uw", host);
        set_optional_supply_counter(lua, names[index], "current_now",
                                   "current_now_ua", host);
        set_optional_supply_counter(lua, names[index], "voltage_now",
                                   "voltage_now_uv", host);
        lua_rawseti(lua, -2, (int)supply_index++);
    }
    lua_setfield(lua, snapshot, "supplies");
    return 1;
}

int i3sd_collect(lua_State *lua, const char *kind, int options,
                 const struct i3sd_collector_host *host) {
    if (strcmp(kind, "cpu") == 0) {
        return sample_cpu(lua, options, host);
    }
    if (strcmp(kind, "load") == 0) {
        return sample_load(lua, options, host);
    }
    if (strcmp(kind, "memory") == 0) {
        return sample_memory(lua, options, host);
    }
    if (strcmp(kind, "filesystem") == 0) {
        return sample_filesystem(lua, options, host);
    }
    if (strcmp(kind, "time") == 0) {
        return sample_time(lua, options, host);
    }
    if (strcmp(kind, "psi") == 0) {
        return sample_psi(lua, options, host);
    }
    if (strcmp(kind, "file_stat") == 0) {
        return sample_file_stat(lua, options, host);
    }
    if (strcmp(kind, "power_supply") == 0) {
        return sample_power_supply(lua, options, host);
    }
    if (strcmp(kind, "nvidia") == 0) {
        return i3sd_nvidia_sample(lua, options, host);
    }
    return luaL_error(lua, "unknown collector '%s'", kind);
}
