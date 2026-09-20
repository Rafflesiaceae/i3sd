#!/bin/sh
set -eu

i3sd_binary="$1"
i3sd_source_dir="$2"

for config in "$i3sd_source_dir"/examples/*.lua; do
    output="$(LUA_PATH="$i3sd_source_dir/lua/?.lua;;" "$i3sd_binary" --check -c "$config")"
    test -z "$output"
done
