#define _GNU_SOURCE

#include "directory_feed.h"

#include "i3sd/utf8.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FEED_MAX_BYTES 4096U
#define FEED_NAME_MAX 255U

/* Only completed writes and renames are observed. IN_CREATE/IN_MODIFY would
 * expose half-written files, e.g. briefly showing the name for "echo x > f". */
#define FEED_EVENTS                                                            \
    (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |                \
     IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

int i3sd_watch_acquire(struct app *app, const char *path, uint32_t mask) {
    /* IN_MASK_ADD keeps the events other consumers of the inode requested. */
    const int wd = inotify_add_watch(app->inotify_fd, path, mask | IN_MASK_ADD);
    if (wd < 0) {
        return -1;
    }
    for (size_t index = 0; index < app->watch_count; index++) {
        if (app->watches[index].wd == wd) {
            app->watches[index].refs++;
            return wd;
        }
    }
    if (app->watch_count == I3SD_MAX_WATCHES) {
        /* Nobody else holds this new descriptor, so drop it again. */
        inotify_rm_watch(app->inotify_fd, wd);
        errno = ENOSPC;
        return -1;
    }
    app->watches[app->watch_count++] =
        (struct watch_entry){.wd = wd, .refs = 1};
    return wd;
}

void i3sd_watch_release(struct app *app, int wd) {
    for (size_t index = 0; index < app->watch_count; index++) {
        struct watch_entry *entry = &app->watches[index];
        if (entry->wd != wd) {
            continue;
        }
        if (--entry->refs == 0) {
            inotify_rm_watch(app->inotify_fd, wd);
            *entry = app->watches[--app->watch_count];
        }
        return;
    }
}

/* The kernel already dropped this watch (IN_IGNORED); never remove it again,
 * because the descriptor number is no longer ours. */
void i3sd_watch_forget(struct app *app, int wd) {
    for (size_t index = 0; index < app->watch_count; index++) {
        if (app->watches[index].wd == wd) {
            app->watches[index] = app->watches[--app->watch_count];
            return;
        }
    }
}

static struct app *feed_app(const struct directory_feed *feed) {
    return feed->block->generation->app;
}

/* Dotfiles and editor backups are ignored, which gives producers an atomic
 * update path: write ".name.tmp", then rename it to "name". */
static bool feed_name(const char *name, size_t len) {
    return len > 0 && len <= FEED_NAME_MAX && name[0] != '.' &&
           name[len - 1] != '~' && i3sd_valid_utf8(name, len);
}

/* Cut a truncated read back to the last complete UTF-8 sequence. */
static size_t utf8_prefix(const char *text, size_t len) {
    for (size_t trim = 0; trim < 4 && trim <= len; trim++) {
        if (i3sd_valid_utf8(text, len - trim)) {
            return len - trim;
        }
    }
    return 0;
}

/* Split at the next newline, dropping a preceding carriage return. */
static char *next_line(char **cursor, char *end) {
    if (*cursor == NULL || *cursor > end) {
        return NULL;
    }
    char *line = *cursor;
    char *newline = memchr(line, '\n', (size_t)(end - line));
    char *line_end = newline != NULL ? newline : end;
    if (line_end > line && line_end[-1] == '\r') {
        line_end--;
    }
    *line_end = '\0';
    *cursor = newline != NULL ? newline + 1 : NULL;
    return line;
}

/* Invalid values leave the last valid setting intact. */
static void parse_boolean(const char *value, bool *setting) {
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *setting = true;
    } else if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *setting = false;
    }
}

/* Tokenize the third line in place: no allocations or generic option map.
 * Unknown keys and malformed values never fault externally supplied items. */
static void parse_options(char *options, struct i3sd_item_text *text) {
    if (options == NULL) {
        return;
    }
    while (*options != '\0') {
        if (*options == ' ' || *options == '\t') {
            options++;
            continue;
        }
        char *key = options;
        while (*options != '\0' && *options != ' ' && *options != '\t') {
            options++;
        }
        if (*options != '\0') {
            *options++ = '\0';
        }
        char *value = strchr(key, '=');
        if (value == NULL) {
            /* A bare color keeps the original third-line format working. */
            if (i3sd_valid_color(key)) {
                text->color = key;
            }
            continue;
        }
        *value++ = '\0';
        if (strcmp(key, "color") == 0) {
            if (i3sd_valid_color(value)) {
                text->color = value;
            }
        } else if (strcmp(key, "pango") == 0) {
            parse_boolean(value, &text->pango);
        } else if (strcmp(key, "urgent") == 0) {
            parse_boolean(value, &text->urgent);
        }
    }
}

static void put_result(struct directory_feed *feed,
                       enum i3sd_put_result result, const char *name) {
    if (result == I3SD_PUT_EVICTED || result == I3SD_PUT_FULL) {
        feed->overflowed = true;
    } else if (result == I3SD_PUT_FAILED) {
        fprintf(stderr, "i3sd: unable to render %s/%s\n", feed->path, name);
    }
}

/* Read one file and update its item. Line one is full_text, line two
 * short_text and line three contains options. An empty file shows its name
 * instead. */
static void refresh_file(struct directory_feed *feed, const char *name,
                         size_t name_len) {
    /* Files are opened by path because a held directory descriptor would pin
     * the directory and delay IN_DELETE_SELF until it is closed. */
    char path[PATH_MAX];
    const int path_len =
        snprintf(path, sizeof(path), "%s/%s", feed->path, name);
    const int fd = path_len > 0 && (size_t)path_len < sizeof(path)
                       ? open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW |
                                        O_CLOEXEC | O_NOCTTY)
                       : -1;
    struct stat metadata;
    if (fd < 0 || fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode)) {
        /* Missing, unreadable or non-regular entries (FIFOs, devices,
         * symlinks) never render; O_NONBLOCK keeps a FIFO from blocking. */
        if (fd >= 0) {
            close(fd);
        }
        if (i3sd_block_remove_item(feed->block, name, name_len) &&
            feed->overflowed && !feed->scanning) {
            i3sd_directory_feed_rescan(feed);
        }
        return;
    }

    char content[FEED_MAX_BYTES + 1];
    size_t len = 0;
    /* fstat already reports the size, so stop there instead of spending a
     * read on EOF; empty files need no read at all. */
    const size_t wanted = (uint64_t)metadata.st_size < feed->max_bytes
                              ? (size_t)metadata.st_size
                              : feed->max_bytes;
    while (len < wanted) {
        const ssize_t count = read(fd, content + len, wanted - len);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            /* Keep the previous item rather than guessing at partial data. */
            close(fd);
            return;
        }
        if (count == 0) {
            break;
        }
        len += (size_t)count;
    }
    close(fd);

    /* Empty files and content that is not UTF-8 show the file name. */
    struct i3sd_item_text text = {.full_text = name};
    len = utf8_prefix(content, len);
    if (metadata.st_size != 0 && len != 0) {
        content[len] = '\0';
        char *cursor = content;
        char *end = content + len;
        char *full_text = next_line(&cursor, end);
        char *short_text = next_line(&cursor, end);
        char *options = next_line(&cursor, end);
        text.full_text = full_text != NULL ? full_text : "";
        text.short_text =
            short_text != NULL && short_text[0] != '\0' ? short_text : NULL;
        parse_options(options, &text);
    }
    put_result(feed, i3sd_block_put_item(feed->block, name, name_len, &text),
               name);
}

struct name_list {
    char (*names)[FEED_NAME_MAX + 1];
    size_t count;
    size_t capacity;
};

/* Keep the lexically first capacity names in sorted order. */
static bool name_list_add(struct name_list *list, const char *name,
                          size_t len) {
    size_t low = 0;
    size_t high = list->count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const char *other = list->names[middle];
        if (i3sd_compare_item_ids(other, strlen(other), name, len) < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == list->capacity) {
        return false;
    }
    const size_t kept =
        list->count < list->capacity ? list->count : list->capacity - 1;
    memmove(list->names[low + 1], list->names[low],
            (kept - low) * sizeof(list->names[0]));
    memcpy(list->names[low], name, len + 1);
    const bool dropped = list->count == list->capacity;
    list->count = kept + 1;
    return !dropped;
}

static bool name_list_contains(const struct name_list *list, const char *name,
                               size_t len) {
    size_t low = 0;
    size_t high = list->count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const char *other = list->names[middle];
        const int order =
            i3sd_compare_item_ids(other, strlen(other), name, len);
        if (order == 0) {
            return true;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

/* Reconcile items with the directory listing. Rescans run only at activation,
 * after an inotify overflow and when a removal may reveal a hidden file;
 * ordinary changes are applied per file. */
void i3sd_directory_feed_rescan(struct directory_feed *feed) {
    if (feed->cancelled || feed->wd < 0) {
        return;
    }
    struct name_list list = {
        .names = calloc(feed->block->max_items, sizeof(*list.names)),
        .capacity = feed->block->max_items,
    };
    const int fd = open(feed->path,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *directory = fd >= 0 ? fdopendir(fd) : NULL;
    if (list.names == NULL || directory == NULL) {
        fprintf(stderr, "i3sd: unable to list %s: %s\n", feed->path,
                strerror(errno));
        if (directory == NULL && fd >= 0) {
            close(fd);
        }
        free(list.names);
        return;
    }

    feed->overflowed = false;
    feed->scanning = true;
    for (struct dirent *entry; (entry = readdir(directory)) != NULL;) {
        const size_t len = strlen(entry->d_name);
        /* d_type avoids opening entries that can never render. */
        if ((entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) &&
            feed_name(entry->d_name, len) &&
            !name_list_add(&list, entry->d_name, len)) {
            feed->overflowed = true;
        }
    }
    closedir(directory);

    struct block *block = feed->block;
    for (size_t index = block->item_count; index-- > 0;) {
        const struct block_item *item = &block->items[index];
        if (!name_list_contains(&list, item->id, item->id_len)) {
            i3sd_block_remove_item(block, item->id, item->id_len);
        }
    }
    for (size_t index = 0; index < list.count; index++) {
        refresh_file(feed, list.names[index], strlen(list.names[index]));
    }
    feed->scanning = false;
    free(list.names);
}

void i3sd_directory_feed_close(struct directory_feed *feed) {
    if (feed->wd >= 0) {
        i3sd_watch_release(feed_app(feed), feed->wd);
        feed->wd = -1;
    }
}

bool i3sd_directory_feed_activate(struct directory_feed *feed) {
    struct app *app = feed_app(feed);
    const char *problem = NULL;
    int dir_fd = -1;
    if (mkdir(feed->path, 0700) < 0 && errno != EEXIST) {
        problem = "unable to create it";
        goto fail;
    }
    /* The descriptor only anchors the checks below and is closed again. */
    dir_fd = open(feed->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat opened;
    if (dir_fd < 0 || fstat(dir_fd, &opened) < 0) {
        problem = "unable to open it";
        goto fail;
    }
    /* Anyone able to write here could inject bar content. Checking the open
     * descriptor rather than the path avoids a check/use race. */
    if (opened.st_uid != geteuid() || (opened.st_mode & 022) != 0) {
        errno = EPERM;
        problem = "it must be owned by this user and not group/world writable";
        goto fail;
    }
    /* Watch before listing so no change can fall between the two. */
    feed->wd = i3sd_watch_acquire(app, feed->path, FEED_EVENTS);
    struct stat watched;
    if (feed->wd < 0) {
        problem = "unable to watch it";
        goto fail;
    }
    if (lstat(feed->path, &watched) < 0 || watched.st_dev != opened.st_dev ||
        watched.st_ino != opened.st_ino) {
        errno = ESTALE;
        problem = "it was replaced while being opened";
        goto fail;
    }
    close(dir_fd);
    i3sd_directory_feed_rescan(feed);
    return true;

fail:
    fprintf(stderr, "i3sd: block %s cannot use directory %s: %s (%s)\n",
            feed->block->name, feed->path, problem, strerror(errno));
    if (dir_fd >= 0) {
        close(dir_fd);
    }
    i3sd_directory_feed_close(feed);
    return false;
}

/* The directory itself went away: show nothing, then recreate it so external
 * producers can keep working without a reload. */
static void lose_directory(struct directory_feed *feed, bool forgotten) {
    if (forgotten) {
        feed->wd = -1;
    }
    i3sd_directory_feed_close(feed);
    i3sd_block_clear_items(feed->block);
    i3sd_directory_feed_activate(feed);
}

void i3sd_directory_feed_event(struct directory_feed *feed,
                               const struct inotify_event *event) {
    if (feed->cancelled || event->wd != feed->wd) {
        return;
    }
    if ((event->mask & IN_IGNORED) != 0) {
        lose_directory(feed, true);
        return;
    }
    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
        lose_directory(feed, false);
        return;
    }
    const size_t len = event->len > 0 ? strlen(event->name) : 0;
    if (!feed_name(event->name, len)) {
        return;
    }
    if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
        /* A removal may reveal a file that did not fit before. */
        if (i3sd_block_remove_item(feed->block, event->name, len) &&
            feed->overflowed) {
            i3sd_directory_feed_rescan(feed);
        }
    } else if ((event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0) {
        refresh_file(feed, event->name, len);
    }
}
