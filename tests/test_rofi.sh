#!/bin/sh
set -eu

i3sd_binary="$1"
i3sd_config="$2"
fake_rofi="$3"
test_dir="$(mktemp -d /tmp/i3sd-rofi-test.XXXXXX)"
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

mkdir "$test_dir/bin"
ln -s "$fake_rofi" "$test_dir/bin/rofi"
mkfifo "$test_dir/input"
mkfifo "$test_dir/output-pipe"
# Opening both FIFO ends keeps stdin alive while the test inspects output.
exec 3<>"$test_dir/input"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!
PATH="$test_dir/bin:$PATH" "$i3sd_binary" -c "$i3sd_config" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!

token=""
attempt=0
while test -z "$token" && test "$attempt" -lt 250; do
    token="$(sed -n 's/.*"instance":"\([^"]*\)".*/\1/p' \
        "$test_dir/output" | tail -n 1)"
    attempt=$((attempt + 1))
    sleep 0.02
done
test -n "$token"
printf '[{"name":"menu","instance":"%s","button":1}]' "$token" >&3

attempt=0
while ! grep -q '"full_text":"second"' "$test_dir/output" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q '"full_text":"second"' "$test_dir/output"
test ! -s "$test_dir/error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
