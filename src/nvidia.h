#ifndef I3SD_NVIDIA_H
#define I3SD_NVIDIA_H

#include "collectors.h"

/* Sample one NVML device and release the process-wide NVML connection. */
int i3sd_nvidia_sample(lua_State *lua, int options,
                       const struct i3sd_collector_host *host);
void i3sd_nvidia_shutdown(void);

#endif
