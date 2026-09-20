#!/bin/sh
set -eu

output="$($1 --check -c "$2")"
test -z "$output"
