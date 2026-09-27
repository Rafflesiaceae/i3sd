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
DEBUG=1 "$i3sd_binary" -c "$test_dir/i3sd.lua" \
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
grep -q 'layout\[0\] block=bottom order=0 declaration=2' "$test_dir/error"
grep -q 'layout\[1\] block=middle order=0 declaration=1' "$test_dir/error"
grep -q 'layout\[2\] block=recovered order=0 declaration=0' "$test_dir/error"

# Creating/truncating a config is not a reload boundary. Keep the previous
# output while a writer still has the replacement file open.
error_frames="$(grep -c '"name":"i3sd-error"' "$test_dir/output" || true)"
output_bytes="$(wc -c <"$test_dir/output")"
rm "$test_dir/i3sd.lua"
exec 4>"$test_dir/i3sd.lua"
printf '%s\n' 'block {' '    name = "direct",' >&4
sleep 0.15
test "$(wc -c <"$test_dir/output")" -eq "$output_bytes"
test "$(grep -c '"name":"i3sd-error"' "$test_dir/output" || true)" -eq "$error_frames"
printf '%s\n' \
    '    update = function(ctx)' \
    '        ctx:set { full_text = "direct" }' \
    '    end,' \
    '}' >&4
exec 4>&-

attempt=0
while ! grep -q '"full_text":"direct"' "$test_dir/output" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q '"full_text":"direct"' "$test_dir/output"
test "$(grep -c '"name":"i3sd-error"' "$test_dir/output" || true)" -eq "$error_frames"

# If the file is replaced while an otherwise valid generation is still staging,
# discard that generation silently and keep the old frame until the newest
# stable snapshot has fully executed.
stage_count="$(grep -c 'i3sd: debug: staging configuration snapshot' \
    "$test_dir/error" || true)"
cat >"$test_dir/i3sd.lua" <<'EOF'
os.execute("sleep 0.2")
block {
    name = "stale",
    update = function(ctx)
        ctx:set { full_text = "stale" }
    end,
}
EOF

attempt=0
while test "$(grep -c 'i3sd: debug: staging configuration snapshot' \
                 "$test_dir/error" || true)" -le "$stage_count" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
test "$(grep -c 'i3sd: debug: staging configuration snapshot' \
          "$test_dir/error" || true)" -gt "$stage_count"

cat >"$test_dir/i3sd.lua" <<'EOF'
block {
    name = "fresh",
    update = function(ctx)
        ctx:set { full_text = "fresh" }
    end,
}
EOF

attempt=0
while ! grep -q '"full_text":"fresh"' "$test_dir/output" && \
      test "$attempt" -lt 250; do
    attempt=$((attempt + 1))
    sleep 0.02
done
grep -q '"full_text":"fresh"' "$test_dir/output"
test "$(grep -c '"name":"i3sd-error"' "$test_dir/output" || true)" -eq "$error_frames"
test "$(grep -c '"full_text":"stale"' "$test_dir/output" || true)" -eq 0
! grep -q 'configuration changed while it was being read' "$test_dir/error"
! grep -q 'configuration became obsolete during staging' "$test_dir/error"

kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
