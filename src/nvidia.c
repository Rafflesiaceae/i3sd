#include "nvidia.h"
#include "config.h"
#include "i3sd/utf8.h"

#include <lauxlib.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t option_index(lua_State *lua, int options) {
    lua_getfield(lua, options, "index");
    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1);
        return 0;
    }
    const lua_Number number = luaL_checknumber(lua, -1);
    const lua_Integer integer = luaL_checkinteger(lua, -1);
    lua_pop(lua, 1);
    if (number != (lua_Number)integer || integer < 0 ||
        (uint64_t)integer > UINT_MAX) {
        luaL_error(lua, "nvidia index must be a non-negative integer");
    }
    return (uint64_t)integer;
}

static void check_options(lua_State *lua, int options) {
    lua_pushnil(lua);
    while (lua_next(lua, options) != 0) {
        if (lua_type(lua, -2) != LUA_TSTRING ||
            strcmp(lua_tostring(lua, -2), "index") != 0) {
            luaL_error(lua, "unknown nvidia option '%s'",
                       lua_type(lua, -2) == LUA_TSTRING ? lua_tostring(lua, -2)
                                                        : "<non-string>");
        }
        lua_pop(lua, 1);
    }
}

#if I3SD_HAVE_NVML
#include <nvml.h>

static bool nvml_initialized;
static uint64_t nvml_continuity;

static void push_error(lua_State *lua, const char *code, const char *message) {
    lua_newtable(lua);
    lua_pushstring(lua, code);
    lua_setfield(lua, -2, "code");
    lua_pushstring(lua, message);
    lua_setfield(lua, -2, "message");
    lua_pushstring(lua, "nvidia");
    lua_setfield(lua, -2, "source");
}

static int sample_error(lua_State *lua, const char *code, nvmlReturn_t result) {
    lua_pushnil(lua);
    push_error(lua, code, nvmlErrorString(result));
    return 2;
}

static bool valid_name(const char *value, size_t capacity) {
    const char *terminator = memchr(value, '\0', capacity);
    return terminator != NULL && terminator != value &&
           i3sd_valid_utf8(value, (size_t)(terminator - value));
}

static nvmlReturn_t ensure_initialized(void) {
    if (nvml_initialized) {
        return NVML_SUCCESS;
    }
    const nvmlReturn_t result = nvmlInit_v2();
    if (result == NVML_SUCCESS) {
        nvml_initialized = true;
        /* Reinitialization invalidates any consumer assumptions about the
         * identity-preserving lifetime of the driver connection. */
        nvml_continuity++;
    }
    return result;
}

static void set_optional_metrics(lua_State *lua, nvmlDevice_t device,
                                 const struct i3sd_collector_host *host) {
    nvmlUtilization_t utilization;
    if (nvmlDeviceGetUtilizationRates(device, &utilization) == NVML_SUCCESS) {
        lua_pushinteger(lua, utilization.gpu);
        lua_setfield(lua, -2, "gpu_utilization_percent");
        lua_pushinteger(lua, utilization.memory);
        lua_setfield(lua, -2, "memory_utilization_percent");
    }

    nvmlMemory_t memory;
    if (nvmlDeviceGetMemoryInfo(device, &memory) == NVML_SUCCESS) {
        host->push_uint64(lua, memory.total);
        lua_setfield(lua, -2, "memory_total_bytes");
        host->push_uint64(lua, memory.used);
        lua_setfield(lua, -2, "memory_used_bytes");
        host->push_uint64(lua, memory.free);
        lua_setfield(lua, -2, "memory_free_bytes");
    }

    unsigned int power_mw;
    if (nvmlDeviceGetPowerUsage(device, &power_mw) == NVML_SUCCESS) {
        lua_pushinteger(lua, power_mw);
        lua_setfield(lua, -2, "power_mw");
    }

    unsigned int temperature;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const nvmlReturn_t temperature_result =
        nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature);
#pragma GCC diagnostic pop
    if (temperature_result == NVML_SUCCESS) {
        lua_pushinteger(lua, temperature);
        lua_setfield(lua, -2, "temperature_celsius");
    }
}

int i3sd_nvidia_sample(lua_State *lua, int options,
                       const struct i3sd_collector_host *host) {
    check_options(lua, options);
    const unsigned int index = (unsigned int)option_index(lua, options);
    nvmlReturn_t result = ensure_initialized();
    if (result != NVML_SUCCESS) {
        return sample_error(lua, "unavailable", result);
    }

    unsigned int device_count;
    result = nvmlDeviceGetCount_v2(&device_count);
    if (result != NVML_SUCCESS) {
        if (result == NVML_ERROR_UNINITIALIZED) {
            nvml_initialized = false;
        }
        return sample_error(lua, "unavailable", result);
    }
    if (index >= device_count) {
        lua_pushnil(lua);
        push_error(lua, "not_found", "NVIDIA GPU index is out of range");
        return 2;
    }

    nvmlDevice_t device;
    result = nvmlDeviceGetHandleByIndex_v2(index, &device);
    if (result != NVML_SUCCESS) {
        return sample_error(lua, "unavailable", result);
    }

    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE] = {0};
    result = nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
    if (result != NVML_SUCCESS || !valid_name(uuid, sizeof(uuid))) {
        return result == NVML_SUCCESS
                   ? sample_error(lua, "unavailable", NVML_ERROR_UNKNOWN)
                   : sample_error(lua, "unavailable", result);
    }

    char source_id[sizeof(uuid) + sizeof("nvml:")];
    snprintf(source_id, sizeof(source_id), "nvml:%s", uuid);
    lua_newtable(lua);
    host->push_uint64(lua, host->now_ns());
    lua_setfield(lua, -2, "timestamp_ns");
    lua_pushstring(lua, source_id);
    lua_setfield(lua, -2, "source_id");
    host->push_uint64(lua, nvml_continuity);
    lua_setfield(lua, -2, "continuity");
    lua_pushinteger(lua, index);
    lua_setfield(lua, -2, "index");
    lua_pushstring(lua, uuid);
    lua_setfield(lua, -2, "uuid");

    char name[NVML_DEVICE_NAME_BUFFER_SIZE] = {0};
    if (nvmlDeviceGetName(device, name, sizeof(name)) == NVML_SUCCESS &&
        valid_name(name, sizeof(name))) {
        lua_pushstring(lua, name);
        lua_setfield(lua, -2, "name");
    }
    /* Individual NVML capabilities vary by GPU, so unavailable fields stay
     * absent while the rest of the snapshot remains useful. */
    set_optional_metrics(lua, device, host);
    return 1;
}

void i3sd_nvidia_shutdown(void) {
    if (nvml_initialized) {
        nvmlShutdown();
        nvml_initialized = false;
    }
}

#else

int i3sd_nvidia_sample(lua_State *lua, int options,
                       const struct i3sd_collector_host *host) {
    (void)host;
    check_options(lua, options);
    (void)option_index(lua, options);
    lua_pushnil(lua);
    lua_newtable(lua);
    lua_pushstring(lua, "unsupported");
    lua_setfield(lua, -2, "code");
    lua_pushstring(lua, "i3sd was built without NVML support");
    lua_setfield(lua, -2, "message");
    lua_pushstring(lua, "nvidia");
    lua_setfield(lua, -2, "source");
    return 2;
}

void i3sd_nvidia_shutdown(void) {}

#endif
