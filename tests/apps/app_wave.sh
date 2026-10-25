#!/bin/sh
# Ex3 capacity test: records when it started, then runs for 3 seconds.
cut -d" " -f1 /proc/uptime > "$RESULT_DIR/start.$$"   # monotonic clock (date can jump on WSL)
sleep 3
