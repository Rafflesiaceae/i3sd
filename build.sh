#!/bin/sh
set -eu

i3sd_build_dir="${I3SD_BUILD_DIR:-build}"

# Meson is idempotent here and leaves an existing configured tree intact.
meson setup "$i3sd_build_dir"
meson compile -C "$i3sd_build_dir"
