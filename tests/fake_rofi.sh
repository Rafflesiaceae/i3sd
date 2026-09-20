#!/bin/sh
set -eu

test "$#" -eq 3
test "$1" = "-dmenu"
test "$2" = "-p"
test "$3" = "Test menu"
# Verify the complete menu input before returning a deterministic selection.
test "$(sed -n '1,2p')" = "$(printf 'first\nsecond')"
printf 'second\n'
