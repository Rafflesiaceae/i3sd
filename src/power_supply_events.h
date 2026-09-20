#ifndef I3SD_POWER_SUPPLY_EVENTS_H
#define I3SD_POWER_SUPPLY_EVENTS_H

#include "runtime.h"

void i3sd_power_supply_close(struct power_supply_source *source);
uint64_t i3sd_power_supply_deadline(const struct power_supply_source *source);
void i3sd_power_supply_notify(struct app *app);
void i3sd_power_supply_process(struct power_supply_source *source,
                              uint64_t now_ns);
void i3sd_power_supply_reconcile(struct power_supply_source *source,
                                 uint64_t now_ns);

#endif
