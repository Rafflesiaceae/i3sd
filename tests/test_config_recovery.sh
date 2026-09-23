#!/bin/sh
set -eu

i3sd_binary="$1"
valid_config="$2"
test_dir="$(mktemp -d /tmp/i3sd-config-recovery.XXXXXX)"
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

# Keep both pipe ends open so i3sd remains alive while the config is repaired.
mkfifo "$test_dir/input"
mkfifo "$test_dir/output-pipe"
exec 3<>"$test_dir/input"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!

# The embedded newline verifies that only the first diagnostic line is shown.
printf '%s\n' 'error("first line\nsecond line")' >"$test_dir/i3sd.lua"
"$i3sd_binary" -c "$test_dir/i3sd.lua" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!

attempt=0
while ! grep -q '"full_text":"ERROR: .*first line"' "$test_dir/output" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q '"full_text":"ERROR: .*first line"' "$test_dir/output"
test "$(grep -c '"name":"i3sd-error"' "$test_dir/output")" -eq 1
test "$(grep -c 'second line' "$test_dir/output")" -eq 0
kill -0 "$i3sd_pid"

# Closing the replacement file triggers the normal inotify reload path.
cp "$valid_config" "$test_dir/i3sd.lua"
attempt=0
while ! grep -q '"full_text":"recovered"' "$test_dir/output" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q '"full_text":"recovered"' "$test_dir/output"

kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
