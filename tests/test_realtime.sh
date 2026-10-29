#!/bin/sh
set -eu

i3sd_binary="$1"
i3sd_config="$2"
test_dir="$(mktemp -d /tmp/i3sd-realtime.XXXXXX)"
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

mkfifo "$test_dir/input"
mkfifo "$test_dir/output-pipe"
# Opening both FIFO ends keeps stdin alive while the test inspects output.
exec 3<>"$test_dir/input"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!
"$i3sd_binary" -c "$i3sd_config" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!

expected='"full_text":"realtime:future,past,staged"'
attempt=0
while ! grep -q "$expected" "$test_dir/output" && test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q "$expected" "$test_dir/output"

# The cancelled timer had its deadline before the future one, so it would
# have faulted the block by now.
test ! -s "$test_dir/error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
