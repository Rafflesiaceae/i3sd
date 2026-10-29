#!/bin/sh
set -eu

i3sd_binary="$1"
reclaim_config="$2"
fault_config="$3"
test_dir="$(mktemp -d /tmp/i3sd-timer-reclaim.XXXXXX)"
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

# Run one configuration until its output contains the expected text, then
# stop the daemon. Its stderr is left in $test_dir/error for inspection.
run_until() {
    config="$1"
    expected="$2"
    rm -f "$test_dir/input" "$test_dir/output-pipe" "$test_dir/output"
    mkfifo "$test_dir/input"
    mkfifo "$test_dir/output-pipe"
    # Opening both FIFO ends keeps stdin alive while the test inspects output.
    exec 3<>"$test_dir/input"
    cat "$test_dir/output-pipe" >"$test_dir/output" &
    copier_pid=$!
    "$i3sd_binary" -c "$config" \
        <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
    i3sd_pid=$!

    attempt=0
    while ! grep -q "$expected" "$test_dir/output" &&
        test "$attempt" -lt 500; do
        attempt=$((attempt + 1))
        sleep 0.02
    done
    if ! grep -q "$expected" "$test_dir/output"; then
        cat "$test_dir/error" >&2
        exit 1
    fi

    kill -TERM "$i3sd_pid"
    wait "$i3sd_pid"
    i3sd_pid=""
    wait "$copier_pid"
    copier_pid=""
    exec 3>&-
}

# Every chain outlives the timer limit without hitting it.
run_until "$reclaim_config" '"full_text":"reclaim:after,cancel,every,realtime"'
test ! -s "$test_dir/error"

# The faulting block is reported, and the other block keeps updating.
run_until "$fault_config" '"full_text":"survivor:ok"'
grep -q "intentional timer fault" "$test_dir/error"
