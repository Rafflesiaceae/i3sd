#include "i3sd/click.h"
#include "i3sd/output.h"
#include "i3sd/timer.h"
#include "i3sd/utf8.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct writer {
    char bytes[256];
    size_t len;
    size_t quantum;
    bool block_next;
};

static ssize_t fake_write(void *userdata, const void *data, size_t len) {
    struct writer *writer = userdata;
    if (writer->block_next) {
        writer->block_next = false;
        errno = EAGAIN;
        return -1;
    }
    if (len > writer->quantum) {
        len = writer->quantum;
    }
    memcpy(writer->bytes + writer->len, data, len);
    writer->len += len;
    return (ssize_t)len;
}

static void test_utf8(void) {
    assert(i3sd_valid_utf8("plain", 5));
    assert(i3sd_valid_utf8("\xc3\xb6", 2));
    assert(!i3sd_valid_utf8("a\0b", 3));
    assert(!i3sd_valid_utf8("\xc0\x80", 2));
    assert(!i3sd_valid_utf8("\xed\xa0\x80", 3));
}

static void test_timers(void) {
    struct i3sd_timer_heap heap = {0};
    struct i3sd_timer first = {0};
    struct i3sd_timer second = {0};
    assert(i3sd_timer_add(&heap, &first, 100, 10, NULL));
    assert(i3sd_timer_add(&heap, &second, 100, 0, NULL));
    assert(i3sd_timer_pop_due(&heap, 99) == NULL);
    assert(i3sd_timer_pop_due(&heap, 100) == &first);
    assert(i3sd_timer_reschedule_fixed(&heap, &first, 135));
    assert(first.deadline_ns == 140);
    assert(i3sd_timer_pop_due(&heap, 100) == &second);
    i3sd_timer_cancel(&heap, &first);
    assert(heap.len == 0);
    i3sd_timer_heap_destroy(&heap);
}

static void test_click_framer(void) {
    struct i3sd_click_framer framer = {0};
    const char *event;
    size_t event_len;
    size_t consumed;
    const char *part1 = " [ {\"name\":\"a";
    assert(i3sd_click_framer_feed(&framer, part1, strlen(part1), &consumed,
                                  &event, &event_len) == I3SD_CLICK_NONE);
    const char *part2 = "\\\"b\",\"nested\":[1,2]} ,";
    assert(i3sd_click_framer_feed(&framer, part2, strlen(part2), &consumed,
                                  &event, &event_len) == I3SD_CLICK_EVENT);
    assert(event_len == strlen("{\"name\":\"a\\\"b\",\"nested\":[1,2]}"));
    assert(memcmp(event, "{\"name\":\"a\\\"b\",\"nested\":[1,2]}", event_len) ==
           0);
    i3sd_click_framer_destroy(&framer);
}

static void test_output(void) {
    struct i3sd_output output;
    struct writer writer = {.quantum = 2};
    i3sd_output_init(&output);
    assert(i3sd_output_set_frame(&output, "[1]", 3));
    while (i3sd_output_has_pending(&output)) {
        i3sd_output_flush(&output, fake_write, &writer, 2, NULL);
    }
    assert(memcmp(writer.bytes, "[1]", 3) == 0);

    /* An identical completed payload does not produce another frame. */
    assert(i3sd_output_set_frame(&output, "[1]", 3));
    assert(!i3sd_output_has_pending(&output));
    assert(i3sd_output_set_frame(&output, "[2]", 3));
    /* Writing the separator and first payload byte makes this frame immutable.
     */
    assert(i3sd_output_flush(&output, fake_write, &writer, 8, NULL) ==
           I3SD_FLUSH_DRAINED);
    assert(i3sd_output_set_frame(&output, "[3]", 3));
    assert(i3sd_output_flush(&output, fake_write, &writer, 2, NULL) ==
           I3SD_FLUSH_BLOCKED);
    writer.block_next = true;
    assert(i3sd_output_set_frame(&output, "[3]", 3));
    assert(output.render_dirty);
    while (i3sd_output_has_pending(&output)) {
        i3sd_output_flush(&output, fake_write, &writer, 8, NULL);
    }
    assert(writer.len == 11);
    assert(memcmp(writer.bytes, "[1],[2],[3]", 11) == 0);
    i3sd_output_destroy(&output);
}

int main(void) {
    test_utf8();
    test_timers();
    test_click_framer();
    test_output();
    puts("core tests passed");
    return 0;
}
