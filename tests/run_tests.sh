#!/usr/bin/env bash
# Exercise 1 test suite for CloudIST.
#
# Usage:  tests/run_tests.sh            (from anywhere)
#
# For every case it checks:
#   - the program exits normally (no crash / signal / timeout)
#   - diff -r <input dir> <VM folder> is empty for every VM. The VM folder is
#     deleted when the VM ends, so the app (tests/apps/app_snapshot.sh) copies
#     its working directory (= the VM folder, after CloudIST's chdir) when it
#     starts, and the test diffs that copy
#   - /tmp/CloudIST-114255/<res> no longer exists after the run (deleted when the VMs end)
#   - the copy was complete when the application started (snapshot taken by
#     tests/tools/startcheck.so at the moment the app is launched)
#   - no file descriptors other than 0/1/2 are open at exit
#   - no memory leaks (valgrind if installed, AddressSanitizer otherwise)
# It also unit-tests copy_path_recursive() directly and checks that the copy
# does not use system()/popen()/"cp -r".

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests"
OUT="$T/out"
IN="$T/inputs"
CLOUD=/tmp/CloudIST-114255
BIN="$ROOT/cloudIST"
ARGS=(2 64 1000 16)          # SERVERS RAM DISK CPU
LOCAL_CORES=4                # only used if the program expects it (see detect_cli)
MAX_THREADS=2                # idem
TAIL=()
TIMEOUT=30

cd "$ROOT" || exit 1

if [ -t 1 ]; then GREEN=$'\e[32m'; RED=$'\e[31m'; YEL=$'\e[33m'; RST=$'\e[0m'; else GREEN=; RED=; YEL=; RST=; fi

PASSED=0; FAILED=0; FAILED_NAMES=()
REASONS=()

fail() { REASONS+=("$*"); }

report() {   # report <name>
  if [ ${#REASONS[@]} -eq 0 ]; then
    printf '%sPASS%s  %s\n' "$GREEN" "$RST" "$1"; PASSED=$((PASSED + 1))
  else
    printf '%sFAIL%s  %s\n' "$RED" "$RST" "$1"; FAILED=$((FAILED + 1)); FAILED_NAMES+=("$1")
    for r in "${REASONS[@]}"; do printf '        - %s\n' "$r"; done
  fi
  REASONS=()
}

# ---------------------------------------------------------------- setup ----

build() {
  echo "== Building"
  make -s -C "$ROOT" clean >/dev/null
  if ! make -s -C "$ROOT" 2> "$OUT/build.log"; then
    cat "$OUT/build.log"; echo "Build failed"; exit 1
  fi
  if [ -s "$OUT/build.log" ]; then echo "${YEL}Build warnings:${RST}"; cat "$OUT/build.log"; else echo "make: no warnings"; fi

  local SRC=(main.c parser.c datacenter.c datacenter_utils.c resources.c filesystem.c)
  gcc -shared -fPIC -O1 -o "$OUT/startcheck.so" "$T/tools/startcheck.c" -ldl || exit 1
  gcc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -o "$OUT/cloudIST_asan" "${SRC[@]}" -lm -pthread || exit 1
  gcc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -o "$OUT/unit_copy" "$T/tools/unit_copy.c" filesystem.c || exit 1

  if command -v valgrind >/dev/null; then
    MEMCHECK=valgrind; echo "memory/fd check: valgrind"
  else
    MEMCHECK=asan; echo "memory/fd check: ${YEL}valgrind not installed${RST} -> AddressSanitizer (leaks) + startcheck.so (fds)"
  fi
  echo
}

gen_inputs() {
  rm -rf "$IN"; mkdir -p "$IN"

  mkdir -p "$IN/flat"
  printf 'hello\n' > "$IN/flat/a.txt"
  printf 'second file\nline 2\n' > "$IN/flat/b.txt"
  printf '' > "$IN/flat/zero_bytes"
  printf '#!/bin/sh\necho hi\n' > "$IN/flat/script.sh"

  mkdir -p "$IN/nested/l1/l2/l3/l4"
  printf 'root\n' > "$IN/nested/root.txt"
  printf 'one\n'  > "$IN/nested/l1/one.txt"
  printf 'two\n'  > "$IN/nested/l1/l2/two.txt"
  printf 'three\n' > "$IN/nested/l1/l2/l3/three.txt"
  printf 'deep\n' > "$IN/nested/l1/l2/l3/l4/deep.txt"
  mkdir -p "$IN/nested/other/x"
  printf 'x\n' > "$IN/nested/other/x/x.txt"

  mkdir -p "$IN/empty_subdir/empty" "$IN/empty_subdir/a/empty2"
  printf 'x\n' > "$IN/empty_subdir/x.txt"

  mkdir -p "$IN/empty_input"

  mkdir -p "$IN/binary"
  for i in $(seq 0 255); do printf "\\$(printf '%03o' "$i")"; done > "$IN/binary/allbytes.bin"
  head -c 65536 /dev/urandom > "$IN/binary/random.bin"
  cp /bin/true "$IN/binary/elf_executable"

  mkdir -p "$IN/large"
  head -c $((5 * 1024 * 1024 + 123)) /dev/urandom > "$IN/large/big.bin"

  mkdir -p "$IN/spaces/dir with spaces"
  printf 'a\n' > "$IN/spaces/my file.txt"
  printf 'b\n' > "$IN/spaces/dir with spaces/inner file.txt"
  printf 'c\n' > "$IN/spaces/  leading.txt"

  mkdir -p "$IN/dest_exists/sub"
  printf 'short\n' > "$IN/dest_exists/a.txt"
  printf 'in sub\n' > "$IN/dest_exists/sub/b.txt"

  # 10_filter needs a subdirectory without a dot in its name (git cannot store it)
  mkdir -p "$T/cases/10_filter/sub_dir"
}

# --------------------------------------------------------------- helpers ---

# run_prog <name> <input_dir_arg>   -> fills $OUT/<name>.{stdout,stderr,rc,log}
run_prog() {
  local name=$1 dir=$2
  rm -rf "$OUT/$name".*
  SNAPSHOT_DIR="$OUT/$name.snap" CLOUDIST_TEST_LOG="$OUT/$name.log" LD_PRELOAD="$OUT/startcheck.so" \
    timeout "$TIMEOUT" "$BIN" "${ARGS[@]}" "$dir" "${TAIL[@]}" < /dev/null \
    > "$OUT/$name.stdout" 2> "$OUT/$name.stderr"
  RC=$?
  echo "$RC" > "$OUT/$name.rc"
  touch "$OUT/$name.log"
}

# memcheck <name> <input_dir_arg> [pre-hook]  (runs the program a second time)
memcheck() {
  local name=$1 dir=$2 hook=${3:-}
  rm -rf "$CLOUD"; [ -n "$hook" ] && $hook
  if [ "$MEMCHECK" = valgrind ]; then
    timeout $((TIMEOUT * 10)) valgrind --leak-check=full --track-fds=yes --show-leak-kinds=all \
      --log-file="$OUT/$name.valgrind" "$BIN" "${ARGS[@]}" "$dir" "${TAIL[@]}" < /dev/null > /dev/null 2>&1
    local lost
    lost=$(grep -E "definitely lost:|indirectly lost:|still reachable:" "$OUT/$name.valgrind" \
           | grep -v " 0 bytes" | sed 's/^==[0-9]*== *//' | paste -sd ';' -)
    [ -n "$lost" ] && fail "valgrind: $lost (see tests/out/$name.valgrind)"
    local fds
    fds=$(grep -oE "FILE DESCRIPTORS: [0-9]+ open \([0-9]+ std\)" "$OUT/$name.valgrind" | head -1)
    if [ -n "$fds" ]; then
      local open std
      open=$(sed -E 's/.*: ([0-9]+) open.*/\1/' <<< "$fds"); std=$(sed -E 's/.*\(([0-9]+) std.*/\1/' <<< "$fds")
      [ "$open" -gt "$std" ] && fail "valgrind: $fds -> unclosed fds (see tests/out/$name.valgrind)"
    fi
    grep -q "Invalid read\|Invalid write\|uninitialised" "$OUT/$name.valgrind" \
      && fail "valgrind: memory errors (see tests/out/$name.valgrind)"
  else
    ASAN_OPTIONS=detect_leaks=1:log_path="$OUT/$name.asan" UBSAN_OPTIONS=print_stacktrace=1 \
      timeout $((TIMEOUT * 3)) "$OUT/cloudIST_asan" "${ARGS[@]}" "$dir" "${TAIL[@]}" < /dev/null > /dev/null 2>&1
    local f
    for f in "$OUT/$name".asan*; do
      [ -e "$f" ] || continue
      local summary
      summary=$(grep -m1 "SUMMARY" "$f" | sed 's/^SUMMARY: //')
      fail "ASan: ${summary:-error} (see ${f#"$ROOT"/})"
    done
  fi
}

# The normal run's startcheck log: report unclosed fds
check_fds() {
  local name=$1
  local fds
  fds=$(grep '^OPENFD' "$OUT/$name.log" | sed 's/^OPENFD //' | paste -sd ',' -)
  [ -n "$fds" ] && fail "file descriptors still open at exit: $fds"
}

check_exit() {   # check_exit <name> [expected: zero|nonzero|any]
  local rc; rc=$(cat "$OUT/$1.rc")
  if [ "$rc" -eq 124 ]; then fail "timeout after ${TIMEOUT}s (hang - maybe waiting on stdin?)"
  elif [ "$rc" -ge 128 ]; then fail "crashed with signal $((rc - 128))"
  elif grep -q "runtime error" "$OUT/$1.stderr"; then fail "UBSan: $(grep -m1 'runtime error' "$OUT/$1.stderr")"
  fi
  case "${2:-any}" in
    zero)    [ "$rc" -ne 0 ] && fail "exit code $rc, expected 0" ;;
    nonzero) [ "$rc" -eq 0 ] && fail "exit code 0, expected an error exit code" ;;
  esac
}

listing() {   # type size path, dirs have size 0
  ( cd "$1" && { find . -mindepth 1 -type d -printf 'd 0 %P\n'; find . -mindepth 1 -type f -printf 'f %s %P\n'; \
                 find . -mindepth 1 -type l -printf 'l 0 %P\n'; } ) | LC_ALL=C sort
}

# snapshots_of <name> <res> <vm>: for every app-start snapshot (exec or the base-code
# stub message; fork is ignored because from exercise 2 on the copy happens in the
# child, after the fork) in which <res>/<vm> exists, print that VM's subtree, sorted,
# followed by a line "@@END".
snapshots_of() {
  awk -v vmdir="$2/$3" -v p="$2/$3/" '
    /^SNAPSHOT/    { inside = ($2 != "fork"); n=0; has=0; next }
    /^ENDSNAPSHOT/ {
      if (inside && has) { cmd = "LC_ALL=C sort"; for (i = 0; i < n; i++) print buf[i] | cmd; close(cmd); print "@@END" }
      inside=0; next
    }
    inside {
      t=$1; s=$2; rest=substr($0, length($1) + length($2) + 3)
      if (rest == vmdir) has=1
      if (index(rest, p) == 1) buf[n++] = t " " s " " substr(rest, length(p) + 1)
    }' "$OUT/$1.log"
}

# snapshot_verdict <name> <res> <vm> <src>: COMPLETE | INCOMPLETE | MISSING | NOSTART
snapshot_verdict() {
  grep -qE '^SNAPSHOT (exec|stub)' "$OUT/$1.log" || { echo NOSTART; return; }
  local expected current="" any=0 line
  expected=$(listing "$4")
  while IFS= read -r line; do
    if [ "$line" = "@@END" ]; then
      any=1
      [ "$current" = "$expected" ] && { echo COMPLETE; return; }
      current=""
    else
      current+="${current:+$'\n'}$line"
    fi
  done < <(snapshots_of "$1" "$2" "$3")
  if [ $any -eq 1 ]; then echo INCOMPLETE; else echo MISSING; fi
}

# check_copy <name> <res> <vm> <src>
check_copy() {
  local name=$1 res=$2 vm=$3 src=$4 dst="$CLOUD/$2/$3" snap="$OUT/$1.snap/$2/$3"
  if [ ! -d "$snap" ]; then
    fail "the app of $vm never ran in its VM folder (no snapshot in ${snap#"$ROOT"/}: VM folder not created, or no chdir into it)"
    return
  fi
  [ -e "$CLOUD/$res" ] && fail "$CLOUD/$res still exists after the run (the VM folder must be deleted when the VM ends)"
  local d
  if ! d=$(diff -r "$src" "$snap" 2>&1); then
    fail "diff -r ${src#"$ROOT"/} $dst (as the app saw it) differs:"
    while IFS= read -r line; do REASONS+=("    $line"); done < <(head -8 <<< "$d")
  fi
  case "$(snapshot_verdict "$name" "$res" "$vm" "$src")" in
    NOSTART)    fail "application for $res was never started (no exec / start event seen)" ;;
    MISSING)    fail "$dst did not exist yet when the application started" ;;
    INCOMPLETE) fail "copy for $vm was INCOMPLETE when the application started" ;;
  esac
}

check_stderr_absent() {  # <name> <pattern> <message>
  grep -q -- "$2" "$OUT/$1.stderr" && fail "$3 (stderr: $(grep -m1 -- "$2" "$OUT/$1.stderr"))"
}

# copy_case <name> <conf_dir> "<res> <vm> <input>"... [--hook fn]
copy_case() {
  local name=$1 dir=$2; shift 2
  local hook="" post=""
  if [ "${1:-}" = --hook ]; then hook=$2; shift 2; fi
  if [ "${1:-}" = --post ]; then post=$2; shift 2; fi
  rm -rf "$CLOUD"; [ -n "$hook" ] && $hook
  run_prog "$name" "$dir"
  check_exit "$name" zero
  [ -n "$post" ] && $post
  local spec
  for spec in "$@"; do
    # shellcheck disable=SC2086
    set -- $spec
    check_copy "$name" "$1" "$2" "$IN/$3"
  done
  check_fds "$name"
  memcheck "$name" "$dir" "$hook"
  report "$name"
}

# The command line changes between exercises:
#   Ex1: <SERVERS> <RAM> <DISK> <CPU> <INPUT_DIR>
#   Ex2: <SERVERS> <RAM> <DISK> <CPU> <LOCAL_CORES> <INPUT_DIR>        (teacher's FAQ)
#   Ex3: <SERVERS> <RAM> <DISK> <CPU> <LOCAL_CORES> <INPUT_DIR> <MAX_THREADS>
# Count the <...> in the program's Usage message to know which one it expects.
detect_cli() {
  local n
  n=$("$BIN" 2>&1 | grep -m1 -i usage | grep -o '<[^>]*>' | wc -l)
  TAIL=()
  [ "$n" -ge 6 ] && ARGS+=("$LOCAL_CORES")
  [ "$n" -ge 7 ] && TAIL=("$MAX_THREADS")
  echo "command line: cloudIST ${ARGS[*]} <INPUT_DIR> ${TAIL[*]}  (Usage shows $n arguments)"
  echo
}

# ------------------------------------------------------------------ main ---

mkdir -p "$OUT"
build
detect_cli
gen_inputs

echo "== Static checks"
if grep -nE '\b(system|popen)\s*\(|"cp |execl?p?\s*\(\s*"cp"|/bin/cp' ./*.c; then
  fail "copy uses system()/popen()/cp"
fi
report "S1 no system()/popen()/cp used for copy"
grep -n "closedir" filesystem.c >/dev/null || fail "filesystem.c never calls closedir() (opendir in copy_path_recursive)"
report "S2 every opendir() has a closedir()"
echo

echo "== Unit tests: copy_path_recursive() called directly"
for d in flat nested empty_subdir empty_input binary large spaces; do
  name="U-$d"; rm -f "$OUT/$name".*
  dst="$OUT/unit_dst/$d"; rm -rf "$OUT/unit_dst/$d"; mkdir -p "$dst"
  res=$(ASAN_OPTIONS=detect_leaks=1:log_path="$OUT/$name.asan" "$OUT/unit_copy" "$IN/$d" "$dst" 2> "$OUT/$name.stderr")
  [[ "$res" == "ret=0 "* ]] || fail "returned non-zero: $res"
  [[ "$res" == *"leaked_fds=0" ]] || fail "fd leak: $res (one per opened directory -> missing closedir)"
  if ! diff -r "$IN/$d" "$dst" > /dev/null 2>&1; then fail "diff -r differs: $(diff -r "$IN/$d" "$dst" 2>&1 | head -3 | paste -sd ';' -)"; fi
  for f in "$OUT/$name".asan*; do [ -e "$f" ] && fail "ASan: $(grep -m1 SUMMARY "$f") (see ${f#"$ROOT"/})"; done
  report "$name (destination pre-created)"
done
name="U-missing-dst"
rm -rf "$OUT/unit_dst/missing"
res=$("$OUT/unit_copy" "$IN/nested" "$OUT/unit_dst/missing/vm" 2> "$OUT/$name.stderr")
if [ -d "$OUT/unit_dst/missing/vm" ]; then
  diff -r "$IN/nested" "$OUT/unit_dst/missing/vm" >/dev/null || fail "destination created but contents differ"
elif [[ "$res" == "ret=0 "* ]]; then
  fail "destination does not exist: nothing copied, but copy_path_recursive returned 0 (error swallowed). stderr: $(head -1 "$OUT/$name.stderr")"
fi
report "$name (destination parent dirs do not exist)"
name="U-missing-src"
res=$("$OUT/unit_copy" "$IN/does_not_exist" "$OUT/unit_dst/x" 2> "$OUT/$name.stderr")
[[ "$res" == "ret=0 "* ]] && fail "returned 0 for a missing source"
report "$name (source does not exist -> error)"
echo

echo "== End-to-end tests (./cloudIST ${ARGS[*]} <INPUT_DIR>)"
C="$T/cases"
copy_case 01_flat          "$C/01_flat"         "T01 T01_1 flat"
copy_case 02_nested        "$C/02_nested"       "T02 T02_1 nested" "T02 T02_2 nested"
copy_case 03_empty_subdir  "$C/03_empty_subdir" "T03 T03_1 empty_subdir"
copy_case 04_empty_input   "$C/04_empty_input"  "T04 T04_1 empty_input"
copy_case 05_binary        "$C/05_binary"       "T05 T05_1 binary"
copy_case 06_large         "$C/06_large"        "T06 T06_1 large"
copy_case 07_spaces        "$C/07_spaces"       "T07 T07_1 spaces"

# The VM folder already exists with old content. Everything in it must be deleted
# (recursively) before copying, so diff -r must show NO leftovers. A symlink to a
# folder OUTSIDE the VM must be removed as a link only: its target must survive.
pre_08() {
  local vm="$CLOUD/T08/T08_1"
  mkdir -p "$vm/sub" "$vm/velha/x/z"
  printf 'THIS OLD CONTENT IS MUCH LONGER THAN THE NEW ONE\n' > "$vm/a.txt"
  printf 'old\n' > "$vm/antigo.txt"
  printf 'old\n' > "$vm/sub/antigo_sub.txt"
  printf 'old\n' > "$vm/velha/x/y.txt"
  rm -rf "$OUT/08_outside"; mkdir -p "$OUT/08_outside"
  printf 'must survive\n' > "$OUT/08_outside/keep.txt"
  ln -s "$OUT/08_outside" "$vm/link_to_outside"
  ln -s /does/not/exist "$vm/broken_link"
}
post_08() {
  [ -f "$OUT/08_outside/keep.txt" ] \
    || fail "the cleanup followed the symlink link_to_outside and deleted files OUTSIDE the VM folder (use lstat, not stat)"
}
copy_case 08_dest_exists   "$C/08_dest_exists"  --hook pre_08 --post post_08 "T08 T08_1 dest_exists"

copy_case 09_multi_conf    "$C/09_multi_conf"   "T09A T09A_1 flat" "T09B T09B_1 nested" "T09C T09C_1 spaces"

name=10_filter
rm -rf "$CLOUD"
run_prog $name "$C/10_filter"
check_exit $name zero
check_stderr_absent $name NOPE_NO_EXTENSION "file without .conf extension (0notes) was processed"
check_stderr_absent $name NOPE_BAK "x.conf.bak was processed"
check_copy $name T10 T10_1 "$IN/flat"
check_fds $name
memcheck $name "$C/10_filter"
report "$name (only *.conf processed, alphabetical)"

name=11_missing_d_input
rm -rf "$CLOUD"
run_prog $name "$C/11_missing_d_input"
check_exit $name any
grep -qi "invalid input folder\|no such file\|not exist" "$OUT/$name.stderr" || fail "no error message about the missing input folder"
[ -e "$CLOUD/T11" ] && fail "$CLOUD/T11 was created although the input folder does not exist"
check_fds $name
memcheck $name "$C/11_missing_d_input"
report "$name (D with missing input folder -> error, no crash)"

name=12_no_confs
run_prog $name "$C/12_no_confs"
check_exit $name zero
check_fds $name
memcheck $name "$C/12_no_confs"
report "$name (INPUT_DIR without .conf files)"

name=13_input_dir_missing
run_prog $name "$T/cases/this_dir_does_not_exist"
check_exit $name nonzero
[ -s "$OUT/$name.stderr" ] || fail "no error message printed"
memcheck $name "$T/cases/this_dir_does_not_exist"
report "$name (INPUT_DIR argument does not exist -> error)"

name=14_input_dir_is_file
run_prog $name "$C/09_multi_conf/a.conf"
check_exit $name nonzero
[ -s "$OUT/$name.stderr" ] || fail "no error message printed"
memcheck $name "$C/09_multi_conf/a.conf"
report "$name (INPUT_DIR argument is a regular file -> error)"

rm -rf "$CLOUD"
echo
echo "== Summary: ${GREEN}$PASSED passed${RST}, ${RED}$FAILED failed${RST}"
[ "$FAILED" -eq 0 ] || printf '   failed: %s\n' "${FAILED_NAMES[*]}"
echo "   logs (stdout/stderr/rc/startcheck/valgrind|asan) in tests/out/"
[ "$FAILED" -eq 0 ]
