#!/bin/sh
# Records what the application sees when it starts (used by test E04 and E10):
#   - the files in its VM folder at that moment (copy must already be complete)
#   - its open file descriptors (the .conf must not be inherited)
#   - its parent process (must be cpulimit in step 6) and the cpulimit command line
#   - its working directory
OUT="$RESULT_DIR/inspect.$$"
VM=/tmp/CloudIST/E04/E04_1
{
  echo "@@LISTING"
  if [ -d "$VM" ]; then
    ( cd "$VM" && { find . -mindepth 1 -type d -printf 'd 0 %P\n'; find . -mindepth 1 -type f -printf 'f %s %P\n'; } ) | LC_ALL=C sort
  else
    echo "NO_VM_DIR"
  fi
  echo "@@FDS"
  for fd in /proc/$$/fd/*; do printf '%s -> %s\n' "${fd##*/}" "$(readlink "$fd")"; done
  echo "@@PARENT_COMM"
  cat /proc/$PPID/comm
  echo "@@PARENT_CMDLINE"
  tr '\0' ' ' < /proc/$PPID/cmdline; echo
  echo "@@PWD"
  pwd
} > "$OUT"
