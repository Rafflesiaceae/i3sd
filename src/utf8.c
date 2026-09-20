#include "i3sd/utf8.h"

#include <stdint.h>

bool i3sd_valid_utf8(const char *text, size_t len) {
    const unsigned char *bytes = (const unsigned char *)text;
    size_t index = 0;

    while (index < len) {
        const unsigned char first = bytes[index++];
        if (first == 0) {
            return false;
        }
        if (first < 0x80) {
            continue;
        }

        unsigned continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            codepoint = first & 0x1fU;
            minimum = 0x80;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            codepoint = first & 0x0fU;
            minimum = 0x800;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            codepoint = first & 0x07U;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation_count > len - index) {
            return false;
        }
        for (unsigned count = 0; count < continuation_count; count++) {
            const unsigned char next = bytes[index++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}
