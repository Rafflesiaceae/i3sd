#!/bin/sh
set -eu

i3sd_binary="$1"
configured_settings="$2"
default_settings="$3"
test_dir="$(mktemp -d /tmp/i3sd-global-settings.XXXXXX)"
i3sd_pid=""
copier_pid=""

cleanup() {
    if test -n "$i3sd_pid"; then
        kill "$i3sd_pid" 2>/dev/null || true
    fi
    if test -n "$copier_pid"; then
        kill "$copier_pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT

wait_for_header() {
    output="$1"
    attempt=0
    while ! grep -q '"click_events"' "$output" 2>/dev/null && \
          test "$attempt" -lt 250; do
        attempt=$((attempt + 1))
        sleep 0.02
    done
    grep -q '"click_events"' "$output"
}

# Config can disable click handling completely, so a closed stdin must not stop
# the process or prevent normal status output.
mkfifo "$test_dir/config-output-pipe"
cat "$test_dir/config-output-pipe" >"$test_dir/config-output" &
copier_pid=$!
"$i3sd_binary" -c "$configured_settings" </dev/null \
    >"$test_dir/config-output-pipe" 2>"$test_dir/config-error" &
i3sd_pid=$!
wait_for_header "$test_dir/config-output"
kill -0 "$i3sd_pid"
grep -q '"click_events":false' "$test_dir/config-output"
grep -q 'global settings click_events=false debug=true' \
    "$test_dir/config-error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""

# An explicit positive CLI flag overrides the disabled config setting. Keep the
# input FIFO open because enabled click handling treats stdin EOF as shutdown.
mkfifo "$test_dir/input"
mkfifo "$test_dir/enabled-output-pipe"
cat "$test_dir/enabled-output-pipe" >"$test_dir/enabled-output" &
copier_pid=$!
exec 3<>"$test_dir/input"
"$i3sd_binary" --click-events --no-debug -c "$configured_settings" <&3 \
    >"$test_dir/enabled-output-pipe" 2>"$test_dir/enabled-error" &
i3sd_pid=$!
wait_for_header "$test_dir/enabled-output"
kill -0 "$i3sd_pid"
grep -q '"click_events":true' "$test_dir/enabled-output"
! grep -q 'i3sd: debug:' "$test_dir/enabled-error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
exec 3>&-

# The negative CLI flag also overrides the normal enabled default.
mkfifo "$test_dir/disabled-output-pipe"
cat "$test_dir/disabled-output-pipe" >"$test_dir/disabled-output" &
copier_pid=$!
"$i3sd_binary" --no-click-events -c "$default_settings" </dev/null \
    >"$test_dir/disabled-output-pipe" 2>"$test_dir/disabled-error" &
i3sd_pid=$!
wait_for_header "$test_dir/disabled-output"
kill -0 "$i3sd_pid"
grep -q '"click_events":false' "$test_dir/disabled-output"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
