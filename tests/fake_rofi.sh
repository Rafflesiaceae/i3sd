#!/bin/sh
set -eu

# Consume the complete menu input before returning a deterministic selection.
sed -n '1,2p' >/dev/null
printf 'second\n'
