#!/bin/sh
# Ex1 tests: copies the VM folder (the app's working directory, after CloudIST's
# chdir) as it is when the app starts, so the test can diff it with the input
# folder after CloudIST deletes the VM folder (when the VM ends).
[ -n "${SNAPSHOT_DIR:-}" ] || exit 0
dest="$SNAPSHOT_DIR/$(basename "$(dirname "$PWD")")/$(basename "$PWD")"
mkdir -p "$dest" && cp -a . "$dest"/
