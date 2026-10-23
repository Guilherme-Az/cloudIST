#!/bin/sh
# Runs for 1 second, then leaves a marker.
sleep 1
echo done > "$RESULT_DIR/sleep1.$$"
