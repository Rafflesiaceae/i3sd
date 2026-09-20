#ifndef I3SD_POWER_PROFILES_H
#define I3SD_POWER_PROFILES_H

#include "runtime.h"

bool i3sd_power_profiles_needed(const struct app *app);
void i3sd_power_profiles_notify(struct app *app);
void i3sd_power_profiles_close(struct power_profiles_source *source);
void i3sd_power_profiles_reconcile(struct power_profiles_source *source,
                                   uint64_t now_ns);
void i3sd_power_profiles_process(struct power_profiles_source *source,
                                 uint64_t now_ns);
bool i3sd_power_profile_set(struct app *app, const char *profile);
bool i3sd_power_profile_known(const struct power_profiles_source *source,
                              const char *profile);

#endif
