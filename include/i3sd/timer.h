#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct i3sd_timer {
    uint64_t deadline_ns;
    uint64_t interval_ns;
    uint64_t sequence;
    size_t heap_index;
    void *userdata;
    bool active;
};

struct i3sd_timer_heap {
    struct i3sd_timer **items;
    size_t len;
    size_t cap;
    uint64_t next_sequence;
};

void i3sd_timer_heap_destroy(struct i3sd_timer_heap *heap);
bool i3sd_timer_add(struct i3sd_timer_heap *heap, struct i3sd_timer *timer,
                    uint64_t deadline_ns, uint64_t interval_ns, void *userdata);
void i3sd_timer_cancel(struct i3sd_timer_heap *heap, struct i3sd_timer *timer);
struct i3sd_timer *i3sd_timer_peek(const struct i3sd_timer_heap *heap);
struct i3sd_timer *i3sd_timer_pop_due(struct i3sd_timer_heap *heap,
                                      uint64_t now_ns);
bool i3sd_timer_reschedule_fixed(struct i3sd_timer_heap *heap,
                                 struct i3sd_timer *timer, uint64_t now_ns);
