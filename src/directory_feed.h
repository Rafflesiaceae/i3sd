#ifndef I3SD_DIRECTORY_FEED_H
#define I3SD_DIRECTORY_FEED_H

#include "runtime.h"

#include <stdint.h>
#include <sys/inotify.h>

/* Refcounted access to the shared inotify instance. */
int i3sd_watch_acquire(struct app *app, const char *path, uint32_t mask);
void i3sd_watch_release(struct app *app, int wd);
void i3sd_watch_forget(struct app *app, int wd);

/* Create, open, verify and watch the directory, then render its files. */
bool i3sd_directory_feed_activate(struct directory_feed *feed);
void i3sd_directory_feed_close(struct directory_feed *feed);
void i3sd_directory_feed_event(struct directory_feed *feed,
                               const struct inotify_event *event);
void i3sd_directory_feed_rescan(struct directory_feed *feed);

#endif
