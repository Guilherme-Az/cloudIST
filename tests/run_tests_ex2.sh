#!/usr/bin/env bash
# Exercise 2 test suite for CloudIST (fork/exec, reaping VMs, limits).
#
# Usage:  tests/run_tests_ex2.sh            (from anywhere)
#
# Every test is tagged with the TODO step that should make it pass:
#   [passo 1-2] A returns right away + fork/exec
#   [passo 3]   copy inside the child, before exec
#   [passo 4]   each VM destroyed when its app ends (waitpid WNOHANG)
#   [passo 5]   program waits for the remaining VMs before exiting
#   [passo 6]   setrlimit (RAM, file size) + cpulimit
# So it's expected that tests of later steps FAIL while you're on earlier ones.
#
# Each VM runs a small app from tests/apps/ that leaves a marker file in
# $RESULT_DIR, so the test knows the app really ran (and what it saw).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tests"
OUT="$T/out/ex2"
CLOUD=/tmp/CloudIST
BIN="$ROOT/cloudIST"
ARGS=(2 64 1000 16)          # SERVERS RAM DISK CPU  (VMs in the cases: 1 GB RAM, 1 GB disk, 8 vCPU)
LOCAL_CORES=4                # only used if the program expects it (see detect_cli)
MAX_THREADS=2                # idem
TAIL=()
TIMEOUT=30

cd "$ROOT" || exit 1

if [ -t 1 ]; then GREEN=$'\e[32m'; RED=$'\e[31m'; YEL=$'\e[33m'; RST=$'\e[0m'; else GREEN=; RED=; YEL=; RST=; fi

PASSED=0; FAILED=0; FAILED_NAMES=(); REASONS=()

fail() { REASONS+=("$*"); }
info() { printf '        %s(info)%s %s\n' "$YEL" "$RST" "$*"; }

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
  rm -rf "$OUT"; mkdir -p "$OUT/apps" "$OUT/inputs/small/sub/deeper"
  make -s -C "$ROOT" clean >/dev/null
  if ! make -s -C "$ROOT" 2> "$OUT/build.log"; then cat "$OUT/build.log"; echo "Build failed"; exit 1; fi
  if [ -s "$OUT/build.log" ]; then echo "${YEL}Build warnings:${RST}"; cat "$OUT/build.log"; else echo "make: no warnings"; fi

  gcc -shared -fPIC -O1 -o "$OUT/startcheck.so" "$T/tools/startcheck.c" -ldl || exit 1
  gcc -std=c17 -D_POSIX_C_SOURCE=200809L -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -o "$OUT/cloudIST_asan" main.c parser.c datacenter.c datacenter_utils.c resources.c filesystem.c -lm || exit 1

  cp "$T"/apps/*.sh "$T/apps/not_executable.txt" "$OUT/apps/"
  gcc -O1 -o "$OUT/apps/ram_hog" "$T/apps/ram_hog.c" || exit 1
  gcc -O1 -o "$OUT/apps/fsize_hog" "$T/apps/fsize_hog.c" || exit 1

  printf 'a\n' > "$OUT/inputs/small/a.txt"
  printf 'b\n' > "$OUT/inputs/small/sub/b.txt"
  head -c 300000 /dev/urandom > "$OUT/inputs/small/sub/deeper/blob.bin"

  if command -v cpulimit >/dev/null; then echo "cpulimit: found"; else echo "${YEL}cpulimit not installed${RST} (sudo apt install cpulimit) -> step 6 tests will fail"; fi
  echo
}

# prepare <case>: copy the case's .conf files into $OUT/<case>/confs, filling in the paths
prepare() {
  local c=$1
  mkdir -p "$OUT/$c/confs" "$OUT/$c/results"
  local f
  for f in "$T/cases_ex2/$c"/*.conf; do
    sed -e "s|@APPS@|$OUT/apps|g" -e "s|@INPUTS@|$OUT/inputs|g" "$f" > "$OUT/$c/confs/${f##*/}"
  done
}

# run_case <case> [binary]: runs cloudIST on the case; sets RC and ELAPSED (seconds, decimal)
run_case() {
  local c=$1 bin=${2:-$BIN}
  prepare "$c"
  rm -rf "$CLOUD"
  local start end
  start=$(date +%s%N)
  RESULT_DIR="$OUT/$c/results" CLOUDIST_TEST_LOG="$OUT/$c/startcheck.log" LD_PRELOAD="$OUT/startcheck.so" \
    timeout "$TIMEOUT" "$bin" "${ARGS[@]}" "$OUT/$c/confs" "${TAIL[@]}" < /dev/null > "$OUT/$c/stdout" 2> "$OUT/$c/stderr"
  RC=$?
  end=$(date +%s%N)
  ELAPSED=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.2f", (e - s) / 1e9 }')
  touch "$OUT/$c/startcheck.log"
}

check_exit() {   # check_exit <case>
  if [ "$RC" -eq 124 ]; then fail "timeout after ${TIMEOUT}s (hang)"
  elif [ "$RC" -ge 128 ]; then fail "crashed with signal $((RC - 128))"
  elif [ "$RC" -ne 0 ]; then fail "exit code $RC, expected 0"
  fi
  grep -q "runtime error" "$OUT/$1/stderr" && fail "UBSan: $(grep -m1 'runtime error' "$OUT/$1/stderr")"
}

check_fds() {   # unclosed fds at exit (reported by startcheck.so)
  local fds
  fds=$(grep '^OPENFD' "$OUT/$1/startcheck.log" | sed 's/^OPENFD //' | paste -sd ',' -)
  [ -n "$fds" ] && fail "file descriptors still open when cloudIST exits: $fds"
}

count_markers() { find "$OUT/$1/results" -name "$2.*" 2>/dev/null | wc -l; }

# lblock <case> <n>: the n-th output of the L command (1-based)
lblock() {
  awk -v n="$2" '/DATA CENTER STATUS/ { k++ } k == n' "$OUT/$1/stdout"
}

running_vms() { grep -c "VM ID :.*State: RUNNING" <<< "$1"; }
reservations_total() { grep -oE '\[ RESERVATIONS \] \([0-9]+ total\)' <<< "$1" | grep -oE '[0-9]+'; }
hosted_on() { grep -oE 'Hosted VMs \([0-9]+\)' <<< "$1" | sed -n "${2}p" | grep -oE '[0-9]+'; }

check_no_duplicates() {   # each success message appears exactly once per D / R in the case
  local c=$1 d r
  d=$(grep -c "VM successfully defined!" "$OUT/$c/stdout")
  r=$(grep -c "Reservation made successfully!" "$OUT/$c/stdout")
  local nd nr
  nd=$(cat "$OUT/$c/confs"/*.conf | grep -c '^D ')
  nr=$(cat "$OUT/$c/confs"/*.conf | grep -c '^R ')
  { [ "$d" -gt "$nd" ] || [ "$r" -gt "$nr" ]; } && \
    fail "duplicated output: \"VM successfully defined!\" x$d (expected $nd), \"Reservation made successfully!\" x$r (expected $nr) -> missing fflush(stdout) before fork, or exit() instead of _exit() in the child"
}

section() {   # section <case> <name> -> lines of the inspect file between @@<name> and the next @@
  local f
  f=$(find "$OUT/$1/results" -name 'inspect.*' | head -1)
  [ -n "$f" ] && awk -v s="@@$2" '/^@@/ { on = ($0 == s); next } on' "$f"
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

build
detect_cli

echo "== Exercise 2 tests"

# ---- E01
run_case e01_running
check_exit e01_running
L1=$(lblock e01_running 1)
if [ -z "$L1" ]; then
  fail "the L command printed nothing"
elif [ "$(running_vms "$L1")" -ne 1 ]; then
  fail "L right after A shows $(running_vms "$L1") VMs RUNNING, expected 1 -> A still waits for the VMs (wait_for_all_vms) or no fork yet"
elif ! grep -qE "PID: [0-9]+" <<< "$L1"; then
  fail "VM is RUNNING but L shows no PID -> set vm->pid in the parent after fork"
fi
check_fds e01_running
report "E01 [passo 1-2] A returns right away: L shows the VM RUNNING with its PID"

# ---- E02
run_case e02_three_vms
check_exit e02_three_vms
n=$(count_markers e02_three_vms quick)
[ "$n" -eq 3 ] || fail "$n of 3 VM applications ran (each app leaves a marker in tests/out/ex2/e02_three_vms/results)"
check_no_duplicates e02_three_vms
check_fds e02_three_vms
report "E02 [passo 2] each of the 3 VMs really runs its application, no duplicated output"

# ---- E03 / E04 (app_inspect)
run_case e04_inspect
check_exit e04_inspect
if [ -z "$(find "$OUT/e04_inspect/results" -name 'inspect.*' 2>/dev/null)" ]; then
  fail "the application never ran"
  report "E03 [passo 3] inputs fully copied into the VM folder when the app starts"
  fail "the application never ran"
  report "E04 [passo 3] the .conf file descriptor is not inherited by the application (O_CLOEXEC)"
else
  seen=$(section e04_inspect LISTING)
  expected=$(cd "$OUT/inputs/small" && { find . -mindepth 1 -type d -printf 'd 0 %P\n'; find . -mindepth 1 -type f -printf 'f %s %P\n'; } | LC_ALL=C sort)
  if [ "$seen" = "NO_VM_DIR" ]; then fail "/tmp/CloudIST/E04/E04_1 did not exist when the app started"
  elif [ "$seen" != "$expected" ]; then fail "the VM folder was incomplete when the app started: $(diff <(echo "$expected") <(echo "$seen") | grep '^[<>]' | head -3 | paste -sd ';' -)"
  fi
  report "E03 [passo 3] inputs fully copied into the VM folder when the app starts"
  leaked=$(section e04_inspect FDS | grep -E '\.conf$' | paste -sd ',' -)
  [ -n "$leaked" ] && fail "the application inherited the .conf file: fd $leaked -> open the .conf with O_RDONLY | O_CLOEXEC"
  report "E04 [passo 3] the .conf file descriptor is not inherited by the application (O_CLOEXEC)"
fi
check_fds e04_inspect

# ---- E05 / E06 (fast + slow VM in the same reservation)
run_case e05_fast_slow
check_exit e05_fast_slow
L1=$(lblock e05_fast_slow 1); L2=$(lblock e05_fast_slow 2)
if [ -z "$L1" ]; then
  fail "the L command printed nothing"
else
  [ "$(reservations_total "$L1")" = 1 ] || fail "after 2 s: $(reservations_total "$L1") reservations, expected 1 (E05 still has a VM running)"
  grep -q "Total VMs: 1" <<< "$L1" || fail "after 2 s: E05 should have 1 VM left (E05_1 finished after 1 s and must be destroyed)"
  [ "$(hosted_on "$L1" 1)" = 0 ] || fail "after 2 s: server 1 still hosts $(hosted_on "$L1" 1) VM(s), expected 0 (E05_1 ended)"
  [ "$(hosted_on "$L1" 2)" = 1 ] || fail "after 2 s: server 2 hosts $(hosted_on "$L1" 2) VM(s), expected 1 (E05_2 still running)"
  [ "$(running_vms "$L1")" = 1 ] || fail "after 2 s: $(running_vms "$L1") VMs RUNNING, expected 1"
fi
report "E05 [passo 4] a VM is destroyed as soon as its app ends; the reservation stays while another VM runs"
if [ -z "$L2" ]; then
  fail "the second L command printed nothing"
else
  [ "$(reservations_total "$L2")" = 0 ] || fail "after 4.5 s: $(reservations_total "$L2") reservations, expected 0 (all VMs ended -> reservation deleted)"
  [ "$(hosted_on "$L2" 1)" = 0 ] && [ "$(hosted_on "$L2" 2)" = 0 ] || fail "after 4.5 s: servers still host VMs ($(hosted_on "$L2" 1) and $(hosted_on "$L2" 2))"
  used=$(grep "Resources Used" <<< "$L2" | grep -vcE ':\s+0\.00 vCPUs \|\s+0 GB RAM \|\s+0 GB Disk')
  [ "$used" -eq 0 ] || fail "after 4.5 s: resources were not given back to the servers ($(grep 'Resources Used' <<< "$L2" | head -1 | sed 's/^ *//'))"
fi
check_fds e05_fast_slow
report "E06 [passo 4] when the last VM ends, the reservation is deleted and all resources return"

# ---- E07 (two .conf files with a 2 s app each)
run_case e07_parallel
check_exit e07_parallel
n=$(count_markers e07_parallel sleep2)
[ "$n" -eq 2 ] || fail "$n of 2 applications had finished when cloudIST exited (step 5: wait for the VMs before exiting)"
awk -v t="$ELAPSED" 'BEGIN { exit !(t < 3.5) }' || fail "took ${ELAPSED}s; the two 2 s apps should run at the same time (~2 s total), not one after the other (~4 s)"
check_fds e07_parallel
report "E07 [passo 2,5] VMs from different .conf files run at the same time (took ${ELAPSED}s)"

# ---- E08 (program must wait before exiting)
run_case e08_wait_at_end
check_exit e08_wait_at_end
n=$(count_markers e08_wait_at_end sleep1)
[ "$n" -eq 1 ] || fail "cloudIST exited after ${ELAPSED}s while the VM (1 s) was still running -> wait for the remaining children at the end of main"
check_fds e08_wait_at_end
report "E08 [passo 5] cloudIST waits for running VMs before exiting"
sleep 1.2   # let a possible orphan app from E08 finish before the next test

# ---- E09 (exec fails)
run_case e09_exec_fails
check_exit e09_exec_fails
check_no_duplicates e09_exec_fails
L1=$(lblock e09_exec_fails 1)
[ -n "$L1" ] && [ "$(reservations_total "$L1")" != 0 ] && fail "the VMs whose exec failed were not destroyed: L still shows $(reservations_total "$L1") reservation(s) (step 4)"
check_fds e09_exec_fails
report "E09 [passo 2,4] exec fails (not executable): no crash, no duplicated output, VMs cleaned up"
# Before step 6 your own exec fails -> your perror should show here. From step 6 on,
# cpulimit is what fails, and with -q it may print nothing; so this is only info.
if [ -s "$OUT/e09_exec_fails/stderr" ]; then info "stderr: $(head -1 "$OUT/e09_exec_fails/stderr")"; else info "stderr is empty (normal if the failure happens inside cpulimit -q)"; fi

# ---- E10 / E11 / E12 (limits)
if [ -z "$(find "$OUT/e04_inspect/results" -name 'inspect.*' 2>/dev/null)" ]; then
  fail "the application never ran (see E03)"
else
  parent=$(section e04_inspect PARENT_COMM)
  if [ "$parent" != "cpulimit" ]; then
    fail "the app's parent process is \"$parent\", expected \"cpulimit\" -> exec cpulimit -q -f -l <pct> -- <exec_path>"
  elif [ ${#ARGS[@]} -lt 5 ]; then
    fail "cloudIST doesn't take <LOCAL_CORES> yet (Usage shows 5 arguments) -> needed for the FAQ formula"
  else
    # FAQ: CPULIMIT = 100 * vCPU * LOCAL_CORES / (CPUs_per_server * SERVERS), truncated (TODO decision 3b)
    expected=$(awk -v v=8 -v l="$LOCAL_CORES" -v c="${ARGS[3]}" -v s="${ARGS[0]}" 'BEGIN { x = int(100 * v * l / (c * s)); print (x < 1 ? 1 : x) }')
    got=$(section e04_inspect PARENT_CMDLINE | grep -oE -- '-l [0-9]+' | awk '{ print $2 }')
    [ "$got" = "$expected" ] || fail "cpulimit -l ${got:-?}, expected $expected = 100 x 8 vCPU x $LOCAL_CORES local cores / (${ARGS[3]} CPUs x ${ARGS[0]} servers)"
  fi
fi
report "E10 [passo 6] the application is launched through cpulimit with the FAQ's CPULIMIT"
[ "${parent:-}" = "cpulimit" ] && info "cpulimit command: $(section e04_inspect PARENT_CMDLINE)"
[ -n "$(section e04_inspect PWD)" ] && info "working directory of the app: $(section e04_inspect PWD) (see TODO doubt 4 about chdir)"

run_case e11_ram
check_exit e11_ram
r=$(cat "$OUT/e11_ram/results"/ram.* 2>/dev/null)
case "$r" in
  LIMITED) ;;
  NOT_LIMITED) fail "the app reserved 1.5 GB in a VM with 1 GB of RAM -> setrlimit(RLIMIT_AS, ram << 30) in the child before exec" ;;
  *) fail "the application never ran" ;;
esac
check_fds e11_ram
report "E11 [passo 6] RAM limit (RLIMIT_AS) is applied"

run_case e12_fsize
check_exit e12_fsize
r=$(cat "$OUT/e12_fsize/results"/fsize.* 2>/dev/null)
case "$r" in
  LIMITED) ;;
  NOT_LIMITED) fail "the app wrote past 1 GB in a VM with 1 GB of disk -> setrlimit(RLIMIT_FSIZE, disk << 30) in the child before exec" ;;
  *) fail "the application never ran" ;;
esac
check_fds e12_fsize
report "E12 [passo 6] file size limit (RLIMIT_FSIZE) is applied"

# ---- E13 memory (AddressSanitizer: valgrind would also follow the fork into the child)
for c in e05_fast_slow e07_parallel; do
  rm -f "$OUT/$c".asan.*
  ASAN_OPTIONS=detect_leaks=1:log_path="$OUT/$c.asan" RESULT_DIR="$OUT/$c/results" \
    timeout "$TIMEOUT" "$OUT/cloudIST_asan" "${ARGS[@]}" "$OUT/$c/confs" "${TAIL[@]}" < /dev/null > /dev/null 2>&1
  for f in "$OUT/$c".asan.*; do
    [ -e "$f" ] && fail "$c: $(grep -m1 SUMMARY "$f" | sed 's/^SUMMARY: //') (see ${f#"$ROOT"/})"
  done
done
report "E13 [geral] no memory leaks or memory errors (AddressSanitizer)"

rm -rf "$CLOUD"
echo
echo "== Summary: ${GREEN}$PASSED passed${RST}, ${RED}$FAILED failed${RST}"
echo "   outputs (stdout/stderr/results of each case) in tests/out/ex2/"
[ "$FAILED" -eq 0 ]
