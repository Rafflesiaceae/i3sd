#!/bin/sh
set -eu

i3sd_binary="$1"
i3sd_source_dir="$2"

for config in "$i3sd_source_dir"/examples/*.lua; do
    # The uninstalled binary must find bundled modules without LUA_PATH.
    output="$("$i3sd_binary" --check -c "$config")"
    test -z "$output"
done
