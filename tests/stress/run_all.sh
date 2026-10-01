#!/usr/bin/env bash
# Reproduce every P8 stress/load measurement. Usage:
#   tests/stress/run_all.sh [stage ...]      stages: editor repo repo_disk index app_idle app_binary app_cycles app_orphan app_soak robust (default: all)
#   QUICK=1  tests/stress/run_all.sh         smoke run (short settle/sampling, 1-minute soak) - NOT valid for reporting
#   SOAK_MIN=20 (default 20) minutes for the soak; DISK_TMP=<dir on a real disk> for the repo_disk stage (default build dir)
# Everything runs inside temporary directories (HN_NOTES_DIR/HN_CONFIG_DIR/HN_STATE_DIR/HN_CACHE_DIR/HN_DATA_DIR are redirected by the tests).
# Needs a Release build:  cmake -S . -B build-p8 -G Ninja -DHN_STRESS=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-p8
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="${BUILD_DIR:-$ROOT/build-p8}"
OUT="${OUT:-$ROOT/build-p8/stress-results/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
export QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1
Q=""; FAST=""; SOAK_MIN="${SOAK_MIN:-20}"
if [ "${QUICK:-0}" = 1 ]; then Q="--quick"; FAST="--fast"; SOAK_MIN=2; fi
DISK_TMP="${DISK_TMP:-$ROOT/build-p8/stress-disk-tmp}"
S="$B/tests/stress"
fails=0
run() { # name cmd...
  local name="$1"; shift
  echo ">>> $name: $*" | tee -a "$OUT/index.txt"
  local t0=$(date +%s)
  "$@" > "$OUT/$name.log" 2>&1; local rc=$?
  local t1=$(date +%s)
  printf '    rc=%d  %ds  pass=%s fail=%s xfail=%s warn=%s skip=%s\n' "$rc" $((t1-t0)) \
    "$(grep -c '^PASS\|^XPASS' "$OUT/$name.log")" "$(grep -c '^FAIL' "$OUT/$name.log")" "$(grep -c '^XFAIL' "$OUT/$name.log")" \
    "$(grep -c '^WARN' "$OUT/$name.log")" "$(grep -c '^SKIP' "$OUT/$name.log")" | tee -a "$OUT/index.txt"
  [ $rc -ne 0 ] && fails=$((fails+1))
}
want() { [ $# -eq 0 ] && return 0; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
STAGES=("$@"); [ ${#STAGES[@]} -eq 0 ] && STAGES=(editor repo repo_disk index app_idle app_binary app_cycles app_orphan app_soak robust)
{ echo "date: $(date -Is)"; echo "kernel: $(uname -srvm)"; echo "cpu: $(grep -m1 'model name' /proc/cpuinfo)"; echo "cores: $(nproc)  mem: $(free -m | awk '/Mem/{print $2" MiB"}')";
  echo "qt: $(pkg-config --modversion Qt6Core 2>/dev/null)  compiler: $(c++ --version | head -1)"; echo "build type: $(grep CMAKE_BUILD_TYPE: "$B/CMakeCache.txt")";
  echo "tmp fs: $(df -T "${TMPDIR:-/tmp}" | tail -1)"; echo "disk-tmp fs: $(mkdir -p "$DISK_TMP"; df -T "$DISK_TMP" | tail -1)"; } > "$OUT/environment.txt"
for st in "${STAGES[@]}"; do
  case $st in
    editor)      run editor "$S/stress_editor" all ;;
    repo)        run repo "$S/stress_repo" all ;;
    repo_disk)   HN_STRESS_TMPDIR="$DISK_TMP" run repo_disk "$S/stress_repo" all ; rm -rf "$DISK_TMP"/hn-* ;;
    index)       run index "$S/stress_index" all ;;
    app_idle)    for n in 1 10 30 100; do run app_idle_$n "$S/stress_app" idle $n $Q; done; run app_idle_10_edited "$S/stress_app" idle 10 --edit $Q ;;
    app_binary)  for n in 10 30 100; do run app_binary_$n "$S/stress_app" binary $n $Q; done ;;
    app_cycles)  run app_cycles "$S/stress_app" cycles $Q ;;
    app_orphan)  run app_orphan "$S/stress_app" orphan ;;
    app_soak)    run app_soak "$S/stress_app" soak "$SOAK_MIN" $FAST ;;
    robust)      run robust "$S/stress_robust" all ;;
    *) echo "unknown stage $st" ;;
  esac
done
echo "results in $OUT (failed executables: $fails)"
grep -h '^FAIL' "$OUT"/*.log | sed 's/^/  /' || true
echo "known-defect (XFAIL) lines:"; grep -h '^XFAIL' "$OUT"/*.log | sed 's/^/  /' || true
exit $((fails>0))
