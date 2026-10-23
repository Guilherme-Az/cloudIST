#!/bin/sh
# Runs for 2 seconds, then leaves a marker.
sleep 2
echo done > "$RESULT_DIR/sleep2.$$"
