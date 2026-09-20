#include "i3sd/timer.h"

#include <stdint.h>
#include <stdlib.h>

static bool timer_before(const struct i3sd_timer *left,
                         const struct i3sd_timer *right) {
    return left->deadline_ns < right->deadline_ns ||
           (left->deadline_ns == right->deadline_ns &&
            left->sequence < right->sequence);
}

static void timer_swap(struct i3sd_timer_heap *heap, size_t left,
                       size_t right) {
    struct i3sd_timer *temporary = heap->items[left];
    heap->items[left] = heap->items[right];
    heap->items[right] = temporary;
    heap->items[left]->heap_index = left;
    heap->items[right]->heap_index = right;
}

static void timer_sift_up(struct i3sd_timer_heap *heap, size_t index) {
    while (index > 0) {
        const size_t parent = (index - 1) / 2;
        if (!timer_before(heap->items[index], heap->items[parent])) {
            break;
        }
        timer_swap(heap, index, parent);
        index = parent;
    }
}

static void timer_sift_down(struct i3sd_timer_heap *heap, size_t index) {
    for (;;) {
        const size_t left = index * 2 + 1;
        const size_t right = left + 1;
        size_t earliest = index;
        if (left < heap->len &&
            timer_before(heap->items[left], heap->items[earliest])) {
            earliest = left;
        }
        if (right < heap->len &&
            timer_before(heap->items[right], heap->items[earliest])) {
            earliest = right;
        }
        if (earliest == index) {
            return;
        }
        timer_swap(heap, index, earliest);
        index = earliest;
    }
}

void i3sd_timer_heap_destroy(struct i3sd_timer_heap *heap) {
    for (size_t index = 0; index < heap->len; index++) {
        heap->items[index]->active = false;
    }
    free(heap->items);
    *heap = (struct i3sd_timer_heap){0};
}

bool i3sd_timer_add(struct i3sd_timer_heap *heap, struct i3sd_timer *timer,
                    uint64_t deadline_ns, uint64_t interval_ns,
                    void *userdata) {
    if (timer->active) {
        return false;
    }
    if (heap->len == heap->cap) {
        size_t next = heap->cap == 0 ? 16 : heap->cap * 2;
        if (next < heap->cap || next > SIZE_MAX / sizeof(*heap->items)) {
            return false;
        }
        struct i3sd_timer **grown =
            realloc(heap->items, next * sizeof(*heap->items));
        if (grown == NULL) {
            return false;
        }
        heap->items = grown;
        heap->cap = next;
    }
    if (++heap->next_sequence == 0) {
        return false;
    }
    *timer = (struct i3sd_timer){
        .deadline_ns = deadline_ns,
        .interval_ns = interval_ns,
        .sequence = heap->next_sequence,
        .heap_index = heap->len,
        .userdata = userdata,
        .active = true,
    };
    heap->items[heap->len++] = timer;
    timer_sift_up(heap, timer->heap_index);
    return true;
}

void i3sd_timer_cancel(struct i3sd_timer_heap *heap, struct i3sd_timer *timer) {
    if (!timer->active) {
        return;
    }
    const size_t index = timer->heap_index;
    timer->active = false;
    heap->len--;
    if (index == heap->len) {
        return;
    }
    heap->items[index] = heap->items[heap->len];
    heap->items[index]->heap_index = index;
    if (index > 0 &&
        timer_before(heap->items[index], heap->items[(index - 1) / 2])) {
        timer_sift_up(heap, index);
    } else {
        timer_sift_down(heap, index);
    }
}

struct i3sd_timer *i3sd_timer_peek(const struct i3sd_timer_heap *heap) {
    return heap->len == 0 ? NULL : heap->items[0];
}

struct i3sd_timer *i3sd_timer_pop_due(struct i3sd_timer_heap *heap,
                                      uint64_t now_ns) {
    struct i3sd_timer *timer = i3sd_timer_peek(heap);
    if (timer == NULL || timer->deadline_ns > now_ns) {
        return NULL;
    }
    i3sd_timer_cancel(heap, timer);
    return timer;
}

bool i3sd_timer_reschedule_fixed(struct i3sd_timer_heap *heap,
                                 struct i3sd_timer *timer, uint64_t now_ns) {
    if (timer->active || timer->interval_ns == 0) {
        return false;
    }
    const uint64_t previous = timer->deadline_ns;
    const uint64_t interval = timer->interval_ns;
    uint64_t periods = 1;
    if (now_ns >= previous) {
        periods = (now_ns - previous) / interval + 1;
    }
    if (periods > (UINT64_MAX - previous) / interval) {
        return false;
    }
    return i3sd_timer_add(heap, timer, previous + periods * interval, interval,
                          timer->userdata);
}
