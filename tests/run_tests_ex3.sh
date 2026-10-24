#!/usr/bin/env bash
# Exercise 3 test suite for CloudIST (worker threads + synchronization).
#
# Usage:  tests/run_tests_ex3.sh            (from anywhere)
#
# Needs the Ex3 command line:
#   cloudIST <SERVERS> <RAM> <DISK> <CPU> <LOCAL_CORES> <INPUT_DIR> <MAX_THREADS>
#
#   X01-X03  MAX_THREADS is respected and .conf files really run in parallel
#   X04      never more than MAX_THREADS worker threads at the same time
#   X05      stress: 40 .conf files at once, everything defined/reserved/run
#   X06      capacity: 20 threads fight for a server with room for 10 VMs ->
#            exactly 10 reservations may succeed (without locks, more can get in)
#   X07      ThreadSanitizer: 0 data races (this is the real test for the locks)
#   X08      AddressSanitizer: no leaks / memory errors under load

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests"
OUT="$T/out/ex3"
CLOUD=/tmp/CloudIST
BIN="$ROOT/cloudIST"
LOCAL_CORES=4
TIMEOUT=60

cd "$ROOT" || exit 1

if [ -t 1 ]; then GREEN=$'\e[32m'; RED=$'\e[31m'; YEL=$'\e[33m'; RST=$'\e[0m'; else GREEN=; RED=; YEL=; RST=; fi

PASSED=0; FAILED=0; REASONS=()
fail() { REASONS+=("$*"); }
info() { printf '        %s(info)%s %s\n' "$YEL" "$RST" "$*"; }
report() {
  if [ ${#REASONS[@]} -eq 0 ]; then
    printf '%sPASS%s  %s\n' "$GREEN" "$RST" "$1"; PASSED=$((PASSED + 1))
  else
    printf '%sFAIL%s  %s\n' "$RED" "$RST" "$1"; FAILED=$((FAILED + 1))
    for r in "${REASONS[@]}"; do printf '        - %s\n' "$r"; done
  fi
  REASONS=()
}

# ---------------------------------------------------------------- setup ----

SRC=(main.c parser.c datacenter.c datacenter_utils.c resources.c filesystem.c)

build() {
  echo "== Building"
  rm -rf "$OUT"; mkdir -p "$OUT/apps" "$OUT/inputs/small/sub"
  make -s -C "$ROOT" clean >/dev/null
  if ! make -s -C "$ROOT" 2> "$OUT/build.log"; then cat "$OUT/build.log"; echo "Build failed"; exit 1; fi
  if [ -s "$OUT/build.log" ]; then echo "${YEL}Build warnings:${RST}"; cat "$OUT/build.log"; else echo "make: no warnings"; fi

  local n
  n=$("$BIN" 2>&1 | grep -m1 -i usage | grep -o '<[^>]*>' | wc -l)
  if [ "$n" -lt 7 ]; then
    echo "${RED}cloudIST's Usage shows $n arguments; the Ex3 tests need 7:${RST}"
    echo "  cloudIST <SERVERS> <RAM> <DISK> <CPU> <LOCAL_CORES> <INPUT_DIR> <MAX_THREADS>"
    exit 1
  fi

  gcc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=thread -o "$OUT/cloudIST_tsan" "${SRC[@]}" -lm -pthread \
    || { echo "TSan build failed"; exit 1; }
  gcc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -o "$OUT/cloudIST_asan" "${SRC[@]}" -lm -pthread || { echo "ASan build failed"; exit 1; }

  cp "$T"/apps/*.sh "$OUT/apps/"
  printf 'a\n' > "$OUT/inputs/small/a.txt"
  printf 'b\n' > "$OUT/inputs/small/sub/b.txt"
  echo
}

# run <case> <max_threads> <servers> <cpus_per_server> [binary] -> RC, ELAPSED, MAX_TASKS
# Samples /proc/<pid>/task while it runs to see how many threads exist at once.
run() {
  local c=$1 mt=$2 servers=$3 cpus=$4 bin=${5:-$BIN}
  rm -rf "$CLOUD" "$OUT/$c/results"; mkdir -p "$OUT/$c/results"
  local start end pid n
  MAX_TASKS=0
  start=$(date +%s%N)
  RESULT_DIR="$OUT/$c/results" timeout "$TIMEOUT" "$bin" "$servers" 64 64 "$cpus" "$LOCAL_CORES" "$OUT/$c/confs" "$mt" \
    < /dev/null > "$OUT/$c/stdout" 2> "$OUT/$c/stderr" &
  pid=$!
  # the timeout process is the parent; its child is cloudIST
  local cpid=""
  while kill -0 "$pid" 2>/dev/null; do
    [ -z "$cpid" ] && cpid=$(pgrep -P "$pid" | head -1)
    if [ -n "$cpid" ] && [ -d "/proc/$cpid/task" ]; then
      n=$(ls "/proc/$cpid/task" 2>/dev/null | wc -l)
      [ "$n" -gt "$MAX_TASKS" ] && MAX_TASKS=$n
    fi
    sleep 0.01
  done
  wait "$pid"; RC=$?
  end=$(date +%s%N)
  ELAPSED=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.2f", (e - s) / 1e9 }')
}

check_exit() {
  if [ "$RC" -eq 124 ]; then fail "timeout after ${TIMEOUT}s (deadlock?)"
  elif [ "$RC" -ge 128 ]; then fail "crashed with signal $((RC - 128))"
  elif [ "$RC" -ne 0 ]; then fail "exit code $RC, expected 0"
  fi
}

between() { awk -v x="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(x >= lo && x <= hi) }'; }

# --------------------------------------------------------------- cases ----

make_cases() {
  local c i
  # 4 .conf files that only wait 1 s each: total time shows how many run at once
  c="$OUT/waits/confs"; mkdir -p "$c"
  for i in 1 2 3 4; do printf '# only waits 1 s\nE 1000\n' > "$c/w$i.conf"; done

  # 40 .conf files, each defines its own type, reserves 1 VM and runs a quick app
  c="$OUT/stress/confs"; mkdir -p "$c"
  for i in $(seq -w 1 40); do
    printf 'D T%s %s/inputs/small %s/apps/app_quick.sh 1 1 1\nR S%s T%s 1 %d\nA S%s\nL\n' \
      "$i" "$OUT" "$OUT" "$i" "$i" $(( (10#$i % 4) + 1 )) "$i" > "$c/s$i.conf"
  done

  # 20 .conf files, each wants 1 VM of 1 vCPU on server 1, which has 10 CPUs.
  # The app runs 3 s, so no VM ends (and gives its CPU back) during the test.
  c="$OUT/capacity/confs"; mkdir -p "$c"
  for i in $(seq -w 1 20); do
    printf 'D C%s %s/inputs/small %s/apps/app_sleep3.sh 1 1 1\nR CAP%s C%s 1 1\nA CAP%s\n' \
      "$i" "$OUT" "$OUT" "$i" "$i" "$i" > "$c/c$i.conf"
  done
}

# ------------------------------------------------------------------ main ---

build
make_cases
echo "== Exercise 3 tests"

# ---- X01-X03: parallelism / MAX_THREADS
run waits 1 2 16; check_exit
between "$ELAPSED" 3.8 6 || fail "MAX_THREADS=1 with 4 waits of 1 s took ${ELAPSED}s, expected ~4 s (one .conf at a time)"
report "X01 MAX_THREADS=1: .conf files run one at a time (took ${ELAPSED}s)"

run waits 2 2 16; check_exit
between "$ELAPSED" 1.8 3.2 || fail "MAX_THREADS=2 with 4 waits of 1 s took ${ELAPSED}s, expected ~2 s (two at a time)"
report "X02 MAX_THREADS=2: two .conf files at a time (took ${ELAPSED}s)"

run waits 4 2 16; check_exit
between "$ELAPSED" 0.8 1.8 || fail "MAX_THREADS=4 with 4 waits of 1 s took ${ELAPSED}s, expected ~1 s (all at once)"
X03_TASKS=$MAX_TASKS
report "X03 MAX_THREADS=4: all four .conf files at once (took ${ELAPSED}s)"

# ---- X04: never more threads than allowed (main thread + MAX_THREADS workers)
run waits 2 2 16; check_exit
[ "$MAX_TASKS" -le 3 ] || fail "saw $MAX_TASKS threads at once with MAX_THREADS=2 (expected at most 1 main + 2 workers)"
[ "$MAX_TASKS" -ge 3 ] || info "only saw $MAX_TASKS threads at once (sampling may have missed one)"
report "X04 never more than MAX_THREADS worker threads (max seen: $MAX_TASKS incl. main; with 4: $X03_TASKS)"

# ---- X05: stress
run stress 8 4 16; check_exit
d=$(grep -c "VM successfully defined!" "$OUT/stress/stdout")
r=$(grep -c "Reservation made successfully!" "$OUT/stress/stdout")
a=$(find "$OUT/stress/results" -name 'quick.*' | wc -l)
[ "$d" -eq 40 ] || fail "$d of 40 VM types defined"
[ "$r" -eq 40 ] || fail "$r of 40 reservations made"
[ "$a" -eq 40 ] || fail "$a of 40 applications ran"
# "No process found" is printed by cpulimit itself when the app ends before it can attach (harmless)
errs=$(grep -vE "^$|^No process found$" "$OUT/stress/stderr" | sort | uniq -c | sort -rn | head -3 | sed 's/^ *//' | paste -sd ';' -)
[ -n "$errs" ] && fail "unexpected errors: $errs"
report "X05 stress: 40 .conf files with 8 threads — all defined, reserved and run"

# ---- X06: capacity race
run capacity 8 1 10; check_exit
ok=$(grep -c "Reservation made successfully!" "$OUT/capacity/stdout")
no=$(grep -c "does not have enough resources" "$OUT/capacity/stderr")
[ "$ok" -eq 10 ] || fail "$ok reservations succeeded on a server with room for 10 -> $( [ "$ok" -gt 10 ] && echo 'two threads took the same CPUs (validate/commit not protected by the same lock)' || echo 'too few: resources lost or wrongly counted' )"
[ "$no" -eq 10 ] || fail "$no reservations were refused for lack of resources, expected 10"
report "X06 capacity: 20 threads, server with room for 10 -> exactly 10 reservations ($ok ok, $no refused)"

# ---- X07: ThreadSanitizer
if ! setarch "$(uname -m)" -R true 2>/dev/null; then
  fail "setarch not available (needed to run ThreadSanitizer on this kernel)"
else
  total=0
  for c in stress capacity; do
    rm -rf "$CLOUD"; mkdir -p "$OUT/$c/results"
    RESULT_DIR="$OUT/$c/results" TSAN_OPTIONS="halt_on_error=0" timeout $((TIMEOUT * 2)) \
      setarch "$(uname -m)" -R "$OUT/cloudIST_tsan" $([ $c = capacity ] && echo "1 64 64 10" || echo "4 64 64 16") \
      "$LOCAL_CORES" "$OUT/$c/confs" 8 < /dev/null > /dev/null 2> "$OUT/$c.tsan"
    n=$(grep -c "WARNING: ThreadSanitizer: data race" "$OUT/$c.tsan")
    total=$((total + n))
    if [ "$n" -gt 0 ]; then
      where=$(grep -A40 "WARNING: ThreadSanitizer: data race" "$OUT/$c.tsan" | grep -E "^    #0 " \
              | grep -vE "pthread_create|malloc|calloc|free|strncmp|memmove|memcpy" \
              | sed -E 's/^ +#0 ([^ ]+) [^ ]*\/([^/ ]+:[0-9]+).*/\1 (\2)/' | sort | uniq -c | sort -rn | head -5 \
              | sed 's/^ *//' | paste -sd ';' -)
      fail "$c: $n data races — in: $where (see ${OUT#"$ROOT"/}/$c.tsan)"
    fi
    grep -q "lock-order-inversion" "$OUT/$c.tsan" && fail "$c: lock-order-inversion (possible deadlock) — locks taken in different orders"
  done
fi
report "X07 ThreadSanitizer: no data races (the real test for the locks)"

# ---- X08: AddressSanitizer under load
rm -f "$OUT"/stress.asan.*
rm -rf "$CLOUD"
RESULT_DIR="$OUT/stress/results" ASAN_OPTIONS=detect_leaks=1:log_path="$OUT/stress.asan" \
  timeout "$TIMEOUT" "$OUT/cloudIST_asan" 4 64 64 16 "$LOCAL_CORES" "$OUT/stress/confs" 8 < /dev/null > /dev/null 2>&1
for f in "$OUT"/stress.asan.*; do
  [ -e "$f" ] && fail "$(grep -m1 SUMMARY "$f" | sed 's/^SUMMARY: //') (see ${f#"$ROOT"/})"
done
report "X08 AddressSanitizer under load: no leaks or memory errors"

rm -rf "$CLOUD"
echo
echo "== Summary: ${GREEN}$PASSED passed${RST}, ${RED}$FAILED failed${RST}"
echo "   outputs in tests/out/ex3/"
[ "$FAILED" -eq 0 ]
