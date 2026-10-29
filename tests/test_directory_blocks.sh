#!/bin/sh
set -eu

i3sd_binary="$1"
source_config="$2"
test_dir="$(mktemp -d /tmp/i3sd-directory-blocks.XXXXXX)"
blocks="$test_dir/blocks"
config="$test_dir/i3sd.lua"
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
export I3SD_TEST_BLOCKS="$blocks"

# Frames are written back to back on one line; print only the newest one.
last_frame() {
    tr -d '\n' <"$test_dir/output" | sed 's/\],\[/]\n[/g' | tail -n 1
}

wait_until() {
    attempt=0
    while ! eval "$1" && test "$attempt" -lt 250; do
        attempt=$((attempt + 1))
        sleep 0.02
    done
    if ! eval "$1"; then
        echo "timed out waiting for: $1" >&2
        last_frame >&2
        exit 1
    fi
}

frame_has() {
    last_frame | grep -q "$1"
}

# A plain "! command" is exempt from set -e, so negations need a function.
frame_lacks() {
    ! frame_has "$1"
}

items() {
    last_frame | grep -o '"name":"directory_blocks"' | wc -l
}

cp "$source_config" "$config"

# Validation must not create the directory.
"$i3sd_binary" --check -c "$config" >"$test_dir/check" 2>&1
test ! -s "$test_dir/check"
test ! -e "$blocks"

mkdir -m 700 "$blocks"
: >"$blocks/10-empty"
printf 'hello\nhi\n#ff0000\nignored\n' >"$blocks/20-text"
printf 'dot' >"$blocks/.dotfile"
printf 'backup' >"$blocks/backup~"
mkfifo "$blocks/05-fifo"
ln -s 20-text "$blocks/06-link"
mkdir "$blocks/07-directory"

mkfifo "$test_dir/input"
mkfifo "$test_dir/output-pipe"
# Opening both FIFO ends keeps stdin alive while the test inspects output.
exec 3<>"$test_dir/input"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!
"$i3sd_binary" -c "$config" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!

# Empty files show their name; others use the i3blocks line convention.
# Dotfiles, backups, FIFOs, symlinks and directories never render.
wait_until 'frame_has "\"full_text\":\"hello\",\"short_text\":\"hi\",\"color\":\"#ff0000\""'
frame_has '"full_text":"10-empty","name":"directory_blocks"'
test "$(items)" -eq 2
token="$(last_frame |
    sed -n 's/.*"instance":"\([0-9a-f]\{32\}\):10-empty".*/\1/p')"
test -n "$token"

# Completed writes and atomic renames update the bar.
printf 'updated\n' >"$blocks/10-empty"
wait_until 'frame_has "\"full_text\":\"updated\""'
printf 'renamed' >"$blocks/.15-new.tmp"
mv "$blocks/.15-new.tmp" "$blocks/15-new"
wait_until 'frame_has "\"full_text\":\"renamed\""'
test "$(items)" -eq 3

# With max_items = 3 the lexically first files win; removing one reveals the
# file that did not fit.
: >"$blocks/01-first"
wait_until 'frame_has "\"full_text\":\"01-first\""'
frame_lacks '"full_text":"hello"'
: >"$blocks/30-last"
rm "$blocks/01-first"
wait_until 'frame_has "\"full_text\":\"hello\""'
frame_lacks '"full_text":"30-last"'
rm "$blocks/30-last"

# Items keep left-to-right file name order.
last_frame | grep -q '"full_text":"updated".*"full_text":"renamed".*"full_text":"hello"'

# A middle click on an item reaches Lua with the file name, which removes it.
printf '[{"name":"directory_blocks","instance":"%s:15-new","button":2}' \
    "$token" >&3
wait_until '! frame_has "\"full_text\":\"renamed\""'
test ! -e "$blocks/15-new"

# Losing the directory clears the items and recreates it for producers.
rm -rf "$blocks"
wait_until 'test -d "$blocks"'
wait_until '! frame_has "directory_blocks"'
printf 'back' >"$blocks/40-back"
wait_until 'frame_has "\"full_text\":\"back\""'

# A reload keeps watching the directory.
{
    cat "$source_config"
    printf '%s\n' 'block {' '    name = "reloaded",' \
        '    update = function(ctx) ctx:set { full_text = "reloaded" } end,' \
        '}'
} >"$config"
wait_until 'frame_has "\"full_text\":\"reloaded\""'
frame_has '"full_text":"back"'
: >"$blocks/50-after-reload"
wait_until 'frame_has "\"full_text\":\"50-after-reload\""'

test ! -s "$test_dir/error"
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""

# Directories others can write to are refused: they could inject content.
chmod 777 "$blocks"
: >"$test_dir/output"
cat "$test_dir/output-pipe" >"$test_dir/output" &
copier_pid=$!
"$i3sd_binary" -c "$config" \
    <&3 >"$test_dir/output-pipe" 2>"$test_dir/error" &
i3sd_pid=$!
wait_until 'grep -q "not group/world writable" "$test_dir/error"'
wait_until 'frame_has "\"full_text\":\"reloaded\""'
frame_lacks 'directory_blocks'
kill -TERM "$i3sd_pid"
wait "$i3sd_pid"
i3sd_pid=""
wait "$copier_pid"
copier_pid=""
