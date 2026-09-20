#!/bin/sh
set -eu

i3sd_build_dir="${I3SD_BUILD_DIR:-build}"
i3sd_sanitize_dir="${I3SD_SANITIZE_DIR:-build-sanitize}"

I3SD_BUILD_DIR="$i3sd_build_dir" ./build.sh
meson test -C "$i3sd_build_dir" --print-errorlogs

# LuaJIT reserves memory in ways LeakSanitizer cannot reliably inspect, while
# AddressSanitizer and UndefinedBehaviorSanitizer remain useful for the core.
meson setup "$i3sd_sanitize_dir" -Db_sanitize=address,undefined
meson compile -C "$i3sd_sanitize_dir"
ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" \
    meson test -C "$i3sd_sanitize_dir" --print-errorlogs
