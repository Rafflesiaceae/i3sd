#ifndef I3SD_SYSTEMD_H
#define I3SD_SYSTEMD_H

#include "runtime.h"

bool i3sd_systemd_bus_needed(const struct app *app, enum systemd_scope scope);
void i3sd_systemd_notify(struct app *app);
void i3sd_systemd_close(struct systemd_bus *source);
bool i3sd_systemd_reconcile(struct systemd_bus *source, uint64_t now_ns);
void i3sd_systemd_process(struct systemd_bus *source, uint64_t now_ns);

#endif
