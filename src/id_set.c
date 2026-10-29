#include "i3sd/id_set.h"

#include <string.h>
#include <xxhash.h>

static_assert((I3SD_ID_SET_SLOTS & (I3SD_ID_SET_SLOTS - 1U)) == 0,
              "slot count must be a power of two");
static_assert(I3SD_ID_SET_SLOTS > I3SD_ID_SET_CAPACITY,
              "a full set must still contain an empty slot");

void i3sd_id_set_init(struct i3sd_id_set *set) {
    /* Entries are only read through occupied slots, so clearing slots is
     * enough and keeps initialization at 1 KiB regardless of capacity. */
    memset(set->slots, 0, sizeof(set->slots));
    set->count = 0;
}

/* Return the slot holding the key, or the empty slot where it belongs. */
static size_t probe(const struct i3sd_id_set *set, const char *key, size_t len,
                    uint64_t hash) {
    size_t slot = (size_t)hash & (I3SD_ID_SET_SLOTS - 1U);
    for (;;) {
        const uint16_t occupant = set->slots[slot];
        if (occupant == 0) {
            return slot;
        }
        const struct i3sd_id_set_entry *entry = &set->entries[occupant - 1U];
        if (entry->hash == hash && entry->len == len &&
            memcmp(entry->key, key, len) == 0) {
            return slot;
        }
        slot = (slot + 1U) & (I3SD_ID_SET_SLOTS - 1U);
    }
}

enum i3sd_id_set_result i3sd_id_set_insert(struct i3sd_id_set *set,
                                           const char *key, size_t len,
                                           size_t value) {
    const uint64_t hash = XXH3_64bits(key, len);
    const size_t slot = probe(set, key, len, hash);
    if (set->slots[slot] != 0) {
        return I3SD_ID_SET_DUPLICATE;
    }
    if (set->count == I3SD_ID_SET_CAPACITY) {
        return I3SD_ID_SET_FULL;
    }
    set->entries[set->count] = (struct i3sd_id_set_entry){
        .key = key,
        .len = len,
        .hash = hash,
        .value = value,
    };
    set->slots[slot] = (uint16_t)(++set->count);
    return I3SD_ID_SET_INSERTED;
}

bool i3sd_id_set_find(const struct i3sd_id_set *set, const char *key,
                      size_t len, size_t *value) {
    const size_t slot = probe(set, key, len, XXH3_64bits(key, len));
    if (set->slots[slot] == 0) {
        return false;
    }
    *value = set->entries[set->slots[slot] - 1U].value;
    return true;
}
