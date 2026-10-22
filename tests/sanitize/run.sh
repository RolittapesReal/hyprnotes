#!/usr/bin/env bash
# Sanitizer and memory-tool gate. Exit status is non-zero if anything finds a problem.
#   tests/sanitize/run.sh asan      ASan + UBSan + LSan over the whole ctest suite (host, GCC, tree build-asan)
#   tests/sanitize/run.sh tsan      ThreadSanitizer over the whole ctest suite (host, GCC, tree build-tsan)
#   tests/sanitize/run.sh valgrind  memcheck over the core/editor/theme/mods/plugin tests (tools container)
#   tests/sanitize/run.sh heap      massif over `stress_app cycles` (tools container) + the live-heap growth check
#   tests/sanitize/run.sh all       all of the above, in that order
# Long jobs run under nice -n 10. Trees are (re)built first if sources changed (build -j8 at most).
# Suppressions: tests/sanitize/tsan.supp and lsan.supp (valgrind.supp if ever needed); every entry has a comment with library and reason.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT" || exit 2
TC="$ROOT/tests/tools/tools-container.sh"
OUT="${HN_SANITIZE_OUT:-$ROOT/build-sanitize-out}"; mkdir -p "$OUT"; export HN_TOOLS_OUT="$OUT"
export QT_QPA_PLATFORM=offscreen
NICE="nice -n 10"
rc=0
usage() { sed -n '2,9p' "${BASH_SOURCE[0]}"; }

# ran_count LOG: number of tests ctest reports as run ("... tests passed ... out of T" -> T), 0 if the line is missing
ran_count() { sed -n 's/^.*tests passed.* out of \([0-9][0-9]*\).*$/\1/p' "$1" | tail -1 | grep . || echo 0; }
need() {   # need NAME COUNT FLOOR: fail when fewer tests ran than expected (wrong regex, unbuilt tree, empty list)
  [ "$2" -ge "$3" ] || { echo "$1: only $2 tests ran, expected at least $3: not trusting this run"; return 1; }
}

build() {   # build TREE: make sure the tree exists and is current
  [ -f "$1/CMakeCache.txt" ] || { echo "run.sh: $1 is not configured (cmake -S . -B $1 -G Ninja -DHN_SANITIZE=address,undefined|thread; see usage below)" >&2; usage >&2; return 1; }
  $NICE cmake --build "$1" -j8 > "$OUT/build-$1.log" 2>&1 || { echo "run.sh: build of $1 failed, see $OUT/build-$1.log" >&2; return 1; }
}

do_asan() {
  build build-asan || return 1
  # leaks in the system Qt/fontconfig go to lsan.supp (with a reason); leaks in Hyprnotes code are fixed, never listed
  local lsan=""; [ -s tests/sanitize/lsan.supp ] && lsan=":suppressions=$ROOT/tests/sanitize/lsan.supp"
  ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1 LSAN_OPTIONS="print_suppressions=0${lsan}" \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    $NICE ctest --test-dir build-asan -j2 --timeout 900 --no-tests=error --output-on-failure > "$OUT/asan.log" 2>&1
  local r=$?; need asan "$(ran_count "$OUT/asan.log")" 55 || r=1; tail -n 4 "$OUT/asan.log"; echo "asan: rc=$r (log $OUT/asan.log)"; return $r
}

# TSan options, see tests/sanitize/tsan.supp for why ignore_interceptors_accesses is on.
TSAN="halt_on_error=1:ignore_interceptors_accesses=1:second_deadlock_stack=1:suppressions=$ROOT/tests/sanitize/tsan.supp"
do_tsan() {
  build build-tsan || return 1
  local r=0
  # the claim in tsan.supp ("thread:QThread::start hides nothing of ours") holds only while src/ creates no QThread of its own
  if grep -rnE 'QThread\b|QThreadPool|QtConcurrent' src --include='*.cpp' --include='*.h' | grep -v '^src/[^:]*:[0-9]*: *//' | grep .; then echo "tsan: src/ now creates Qt threads: review the thread: entry in tsan.supp"; r=1; fi
  TSAN_OPTIONS="$TSAN" $NICE ctest --test-dir build-tsan -j1 --timeout 900 --no-tests=error --output-on-failure -E '^(plugins_market_client|app_market_test)$' > "$OUT/tsan.log" 2>&1 || r=1
  need tsan "$(ran_count "$OUT/tsan.log")" 50 || r=1
  tail -n 4 "$OUT/tsan.log"
  # libtsan 16.2 aborts ("CHECK failed: sanitizer_thread_registry.cpp:186") when Qt starts a second QNetworkAccessManager thread in the same
  # process; a 20-line Qt-only program reproduces it, so it is a TSan runtime bug and not ours. These two tests run one test function per
  # process instead. Two functions create two managers inside ONE function and cannot run at all under this libtsan (they run under ASan).
  local skip=" second_open_shows_cache_then_revalidates offline_with_cache_lists_and_says_cached "
  local bin f
  local funcs ran1 floor
  for bin in build-tsan/src/plugins/plugins_market_client_test build-tsan/src/app/app_market_test; do
    [ -x "$bin" ] || { echo "tsan: $bin is missing"; r=1; continue; }
    funcs=$("$bin" -functions | sed 's/()//' | grep -Ev '^(initTestCase|cleanupTestCase)$'); ran1=0
    for f in $funcs; do
      case "$skip" in *" $f "*) continue;; esac
      ran1=$((ran1 + 1))
      TSAN_OPTIONS="$TSAN" $NICE "$bin" "$f" > "$OUT/tsan-one.log" 2>&1 || { r=1; echo "tsan: FAIL $bin $f"; tail -n 30 "$OUT/tsan-one.log"; }
    done
    case "$bin" in *client*) floor=15;; *) floor=25;; esac   # 19 and 29 functions exist; 2 are skipped
    need "tsan $bin" "$ran1" "$floor" || r=1
  done
  echo "tsan: rc=$r (log $OUT/tsan.log)"; return $r
}

container_tree() {   # build the RelWithDebInfo tree "valgrind" in the container (-O1 -g keeps valgrind stacks readable)
  $NICE "$TC" build valgrind -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-O1 -g" -DHN_STRESS=ON > "$OUT/build-valgrind.log" 2>&1 \
    || { echo "run.sh: container build failed, see $OUT/build-valgrind.log" >&2; return 1; }
}

do_valgrind() {
  container_tree || return 1
  $NICE "$TC" run valgrind -- bash /work/tests/sanitize/valgrind-in-container.sh | tee "$OUT/valgrind-summary.txt"
  local r=${PIPESTATUS[0]}; echo "valgrind: rc=$r (per-test logs $OUT/valgrind-*.log)"; return $r
}

do_heap() {
  # heaptrack is not available here. Leak check = (1) the real in-process lifecycle stress, 10,000 open/type/close cycles on the Release
  # tree, which samples live heap (mallinfo2 after malloc_trim), PSS and widget/fd/thread counts ten times along the run, and asserts that
  # the extended phase keeps the heap flat (< 2 KiB/cycle) and counts back at baseline; (2) valgrind massif over the same cycles in the
  # container, with a per-call-site comparison of two quiescent snapshots.
  local r=0
  build build-stress || return 1
  QT_FORCE_STDERR_LOGGING=1 $NICE build-stress/tests/stress/stress_app cycles 10000 > "$OUT/heap-cycles10000.log" 2>&1 || r=1
  grep -E "^(FAIL|PASS)|curve same20|extended same20" "$OUT/heap-cycles10000.log" | cut -c1-220
  echo "heap (host, 10000 cycles): rc=$r (log $OUT/heap-cycles10000.log)"
  container_tree || return 1
  $NICE "$TC" run valgrind -- bash /work/tests/sanitize/heap-in-container.sh | tee "$OUT/heap-summary.txt"
  [ "${PIPESTATUS[0]}" = 0 ] || r=1
  echo "heap: rc=$r"; return $r
}

case "${1:-}" in
  asan) do_asan; rc=$?;;
  tsan) do_tsan; rc=$?;;
  valgrind) do_valgrind; rc=$?;;
  heap) do_heap; rc=$?;;
  all) do_asan || rc=1; do_tsan || rc=1; do_valgrind || rc=1; do_heap || rc=1;;
  *) usage; exit 2;;
esac
exit $rc
