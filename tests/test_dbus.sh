#!/bin/sh
set -eu

i3sd_binary=$1
config=$2
busctl_binary=$3
test_dir=$(mktemp -d)
i3sd_pid=
reader_pid=

cleanup() {
    if [ -n "$i3sd_pid" ]; then
        kill -TERM "$i3sd_pid" 2>/dev/null || true
        wait "$i3sd_pid" 2>/dev/null || true
    fi
    if [ -n "$reader_pid" ]; then
        wait "$reader_pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT INT TERM

mkfifo "$test_dir/input" "$test_dir/output"
# Opening the input FIFO read/write keeps i3sd's pollable stdin alive.
exec 3<>"$test_dir/input"
cat "$test_dir/output" >"$test_dir/captured" &
reader_pid=$!
"$i3sd_binary" -c "$config" <"$test_dir/input" >"$test_dir/output" \
    2>"$test_dir/error" &
i3sd_pid=$!

wait_for_output() {
    expected=$1
    attempts=0
    while [ "$attempts" -lt 100 ]; do
        if grep -Fq "$expected" "$test_dir/captured"; then
            return 0
        fi
        if ! kill -0 "$i3sd_pid" 2>/dev/null; then
            cat "$test_dir/error" >&2
            return 1
        fi
        attempts=$((attempts + 1))
        sleep 0.05
    done
    cat "$test_dir/error" >&2
    return 1
}

wait_for_output '"full_text":"dbus-ready"'
"$busctl_binary" --user emit /org/i3sd/Test org.i3sd.Test Changed 'a{sv}' 2 \
    greeting s hello count u 42
wait_for_output '"full_text":"hello:42"'

kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=
exec 3>&-
wait "$reader_pid"
reader_pid=

test ! -s "$test_dir/error"
