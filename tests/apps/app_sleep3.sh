#!/bin/sh
# Runs for 3 seconds, then leaves a marker.
sleep 3
echo done > "$RESULT_DIR/sleep3.$$"
