#!/bin/sh
set -eu

i3sd_binary="$1"
i3sd_config="$2"
budget_config="$3"
test_dir="$(mktemp -d /tmp/i3sd-item-lists.XXXXXX)"
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

wait_for() {
    attempt=0
    while ! grep -q "$1" "$test_dir/output" && test "$attempt" -lt 250; do
        attempt=$((attempt + 1))
        sleep 0.02
    done
    grep -q "$1" "$test_dir/output"
}

# Staging validates every rejected list form; the item budget is static.
test -z "$("$i3sd_binary" --check -c "$i3sd_config")"
if "$i3sd_binary" --check -c "$budget_config" 2>"$test_dir/budget-error"; then
    echo "item budget overflow was accepted" >&2
    exit 1
fi
grep -q 'blocks reserve more than 256 items in total' "$test_dir/budget-error"

mkfifo "$test_dir/input"
mkfifo "$test_dir/output-pipe"
# Opening both FIFO ends keeps stdin alive while the test inspects output.
exec 3<>"$test_dir/input"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!
"$i3sd_binary" -c "$i3sd_config" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!

wait_for '"full_text":"anonymous"'
frame="$(grep '"full_text":"anonymous"' "$test_dir/output" | tail -n 1)"
token="$(printf '%s\n' "$frame" |
    sed -n 's/.*"instance":"\([0-9a-f]\{32\}\):vpn".*/\1/p')"
test -n "$token"

# Items render in list order, hidden items are skipped and each item carries
# its own routable instance next to the single-state block.
printf '%s\n' "$frame" | grep -q \
    "\"full_text\":\"vpn\",\"name\":\"list\",\"instance\":\"$token:vpn\".*\"full_text\":\"mail\",\"name\":\"list\",\"instance\":\"$token:mail\".*\"full_text\":\"anonymous\",\"name\":\"list\",\"instance\":\"$token#3\""
test "$(printf '%s\n' "$frame" | grep -o '"name":"list"' | wc -l)" -eq 3
printf '%s\n' "$frame" | grep -q '"full_text":"42","name":"single"'
single="$(printf '%s\n' "$frame" |
    sed -n 's/.*"name":"single","instance":"\([0-9a-f]\{32\}\)".*/\1/p')"
test -n "$single"

printf '[{"name":"list","instance":"%s:vpn","button":1}' "$token" >&3
wait_for '"full_text":"clicked:vpn"'
printf ',{"name":"list","instance":"%s#3","button":1}' "$token" >&3
wait_for '"full_text":"clicked:3"'

# Stale and malformed instances are dropped instead of reaching Lua.
for suffix in ':gone' ':hidden' '#4' '#9' '#0' '' ':'; do
    printf ',{"name":"list","instance":"%s%s","button":1}' \
        "$token" "$suffix" >&3
done
printf ',{"name":"single","instance":"%s","button":1}' "$single" >&3
wait_for '"full_text":"single-clicked"'
test "$(grep -o '"full_text":"clicked:[^"]*"' "$test_dir/output" |
    sort -u | tr '\n' ' ')" = \
    '"full_text":"clicked:3" "full_text":"clicked:vpn" '

test ! -s "$test_dir/error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
