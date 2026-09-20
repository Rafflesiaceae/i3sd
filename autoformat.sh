#!/bin/sh
set -eu

# Keep native code deterministic without formatting generated build output.
rg --files include src tests -g '*.[ch]' -0 | xargs -0 clang-format -i
meson format -i meson.build
