#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The capacity equals the per-frame item budget, so one set can index every
 * item a block may own. Twice as many slots keeps linear probes short. */
#define I3SD_ID_SET_CAPACITY 256U
#define I3SD_ID_SET_SLOTS 512U

enum i3sd_id_set_result {
    I3SD_ID_SET_INSERTED,
    I3SD_ID_SET_DUPLICATE,
    I3SD_ID_SET_FULL,
};

/* Fixed-size open-addressing map from borrowed byte strings to indexes. Keys
 * are not copied and must outlive the set. It is meant for short-lived stack
 * use while reconciling item lists, so it never allocates. */
struct i3sd_id_set {
    uint16_t slots[I3SD_ID_SET_SLOTS]; /* entry index + 1; zero is empty */
    struct i3sd_id_set_entry {
        const char *key;
        size_t len;
        uint64_t hash;
        size_t value;
    } entries[I3SD_ID_SET_CAPACITY];
    size_t count;
};

void i3sd_id_set_init(struct i3sd_id_set *set);
enum i3sd_id_set_result i3sd_id_set_insert(struct i3sd_id_set *set,
                                           const char *key, size_t len,
                                           size_t value);
bool i3sd_id_set_find(const struct i3sd_id_set *set, const char *key,
                      size_t len, size_t *value);
