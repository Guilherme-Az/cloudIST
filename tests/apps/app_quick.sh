#!/bin/sh
# Ends right away. Leaves a marker so the test can count how many VMs really ran.
echo ran > "$RESULT_DIR/quick.$$"
