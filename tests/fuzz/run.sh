#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${HN_FUZZ_SELFTEST:-0}" == 1 && -n "${HN_FUZZ_TEST_ROOT:-}" ]]; then ROOT="$HN_FUZZ_TEST_ROOT"; fi
MINUTES="${1:-}"
if [[ ! "$MINUTES" =~ ^[1-9][0-9]*$ ]]; then
  printf 'usage: %s MINUTES (positive integer per target)\n' "$0" >&2
  exit 2
fi
BUILD="${BUILD:-$ROOT/build-fuzz}"
FUZZ_DIR="$BUILD/tests/fuzz"
LOG_DIR="$BUILD/fuzz-logs"
names=(markdown_roundtrip frontmatter links query_spec fts_query manifest plugin_archive theme_import config ipc_transport hypr_events session market_index)
seconds=$((MINUTES * 60))
if [[ "${HN_FUZZ_SELFTEST:-0}" == 1 ]]; then seconds="${HN_FUZZ_TEST_SECONDS:-1}"; fi
reject_unsafe_sanitizer_options() {
  local variable="$1" options="$2" entry key value lower_key lower_value
  local -a entries=()
  IFS=: read -r -a entries <<< "$options"
  for entry in "${entries[@]}"; do
    [[ "$entry" == *=* ]] || continue
    key="${entry%%=*}"
    value="${entry#*=}"
    lower_key="${key,,}"
    lower_value="${value,,}"
    case "$lower_key" in
      *suppress*|*ignore*|interceptor_via_fun|interceptor_via_lib)
        printf 'strict fuzz mode rejects %s option %s=%s (suppression/ignore option)\n' "$variable" "$key" "$value" >&2
        return 1
        ;;
    esac
    if [[ "$lower_key" == exitcode && "$lower_value" == 0 ]]; then
      printf 'strict fuzz mode rejects %s option %s=%s (makes sanitizer failures exit successfully)\n' "$variable" "$key" "$value" >&2
      return 1
    fi
    case "$variable:$lower_key:$lower_value" in
      ASAN_OPTIONS:alloc_dealloc_mismatch:0|ASAN_OPTIONS:detect_leaks:0|ASAN_OPTIONS:strict_string_checks:0|ASAN_OPTIONS:detect_stack_use_after_return:0|ASAN_OPTIONS:halt_on_error:0|\
      LSAN_OPTIONS:detect_leaks:0|LSAN_OPTIONS:leak_check_at_exit:0|LSAN_OPTIONS:max_leaks:*|LSAN_OPTIONS:use_*:0|\
      UBSAN_OPTIONS:halt_on_error:0|UBSAN_OPTIONS:silence_unsigned_overflow:1|UBSAN_OPTIONS:silence_float_division_by_zero:1|UBSAN_OPTIONS:silence_float_cast_overflow:1)
        printf 'strict fuzz mode rejects %s option %s=%s (weakens required sanitizer checks)\n' "$variable" "$key" "$value" >&2
        return 1
        ;;
    esac
    if [[ "$variable" == LSAN_OPTIONS && "$lower_key" == max_leaks && "$lower_value" =~ ^[1-9][0-9]*$ ]]; then
      printf 'strict fuzz mode rejects %s option %s=%s (limits leak reports)\n' "$variable" "$key" "$value" >&2
      return 1
    fi
  done
}
for sanitizer_variable in ASAN_OPTIONS LSAN_OPTIONS UBSAN_OPTIONS; do
  if ! reject_unsafe_sanitizer_options "$sanitizer_variable" "${!sanitizer_variable-}"; then exit 2; fi
done
verify_corpus_cap() {
  local corpus="$1"
  python3 - "$corpus" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
files = [p for p in root.rglob("*") if p.is_file()]
size = sum(p.stat().st_size for p in files)
if len(files) >= 200 or size >= 1024 * 1024:
    print(f"FAIL corpus cap: {root}: {len(files)} files, {size} bytes (limits: fewer than 200 files, fewer than 1048576 bytes)", file=sys.stderr)
    raise SystemExit(1)
PY
}
set_active_sanitizers() {
  # Append required active values so inherited weaker values cannot win.
  export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}alloc_dealloc_mismatch=1:detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1"
  export LSAN_OPTIONS="${LSAN_OPTIONS:+$LSAN_OPTIONS:}detect_leaks=1"
  export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"
}
mkdir -p "$LOG_DIR" "$BUILD/fuzz-tmp"
corpus_cap_failed=0
for name in "${names[@]}"; do
  binary="$FUZZ_DIR/fuzz_$name"
  corpus="$ROOT/tests/fuzz/corpus/$name"
  [[ -x "$binary" ]] || { printf 'missing fuzz binary: %s\n' "$binary" >&2; exit 1; }
  [[ -d "$corpus" ]] || { printf 'missing corpus: %s\n' "$corpus" >&2; exit 1; }
  has_seed=0
  for seed in "$corpus"/*; do [[ -f "$seed" ]] && { has_seed=1; break; }; done
  (( has_seed )) || { printf 'empty corpus: %s\n' "$corpus" >&2; exit 1; }
  verify_corpus_cap "$corpus"
done
pids=()
declare -A pid_target=()
campaign_failed=0
cleanup() {
  local p
  trap - INT TERM HUP EXIT
  for p in "${pids[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null || true; done
  for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
  pids=()
}
trap cleanup INT TERM HUP
trap cleanup EXIT
wait_batch() {
  local finished status p completed_name
  local -a remaining=("${pids[@]}")
  while (( ${#remaining[@]} )); do
    if wait -n -p finished "${remaining[@]}"; then status=0; else status=$?; fi
    local -a next=()
    for p in "${remaining[@]}"; do [[ "$p" == "${finished:-}" ]] || next+=("$p"); done
    remaining=("${next[@]}")
    completed_name="${pid_target[${finished:-}]:-}"
    unset 'pid_target['"${finished:-}"']'
    if (( status == 0 )) && [[ -n "$completed_name" ]]; then
      if ! verify_run "$completed_name"; then status=1; fi
    fi
    if (( status != 0 )); then
      campaign_failed=1
      for p in "${remaining[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null || true; done
      for p in "${remaining[@]}"; do wait "$p" 2>/dev/null || true; done
      break
    fi
  done
  pids=()
  return 0
}
verify_run() {
  local name="$1" log="$LOG_DIR/$1.log" runtime="$LOG_DIR/$1.runtime"
  local ran units cov ft
  local bad=0
  ran="$(cat "$runtime" 2>/dev/null || printf 0)"
  if ! python3 - "$ran" "$seconds" <<'PY'
import sys
try: ok = float(sys.argv[1]) >= float(sys.argv[2])
except ValueError: ok = False
raise SystemExit(0 if ok else 1)
PY
  then printf 'FAIL %s: ran %ss, requested %ss\n' "$name" "$ran" "$seconds" >&2; campaign_failed=1; bad=1; fi
  units="$(sed -nE 's/.*stat::number_of_executed_units:[[:space:]]*([0-9]+).*/\1/p' "$log" | tail -1)"
  cov="$(sed -nE 's/.*cov:[[:space:]]*([0-9]+).*/\1/p' "$log" | tail -1)"
  ft="$(sed -nE 's/.*ft:[[:space:]]*([0-9]+).*/\1/p' "$log" | tail -1)"
  if [[ ! "$units" =~ ^[1-9][0-9]*$ || ! "$cov" =~ ^[1-9][0-9]*$ || ! "$ft" =~ ^[1-9][0-9]*$ ]]; then
    printf 'FAIL %s: missing positive libFuzzer units/cov/ft stats (units=%s cov=%s ft=%s)\n' "$name" "${units:-missing}" "${cov:-missing}" "${ft:-missing}" >&2
    campaign_failed=1
    bad=1
  fi
  return "$bad"
}
for name in "${names[@]}"; do
  binary="$FUZZ_DIR/fuzz_$name"
  corpus="$ROOT/tests/fuzz/corpus/$name"
  regressions="$ROOT/tests/fuzz/regressions/$name"
  mkdir -p "$regressions" "$BUILD/fuzz-work/$name"
  cp -n "$corpus"/* "$BUILD/fuzz-work/$name/" 2>/dev/null || true
  (export TMPDIR="$BUILD/fuzz-tmp" QT_QPA_PLATFORM=offscreen
   set_active_sanitizers
    start="$(python3 -c 'import time; print(time.monotonic())')"
    setsid nice -n 10 timeout --signal=INT --kill-after=15s "$(awk -v s="$seconds" 'BEGIN { printf "%.3fs", s+15 }')" "$binary" \
       -max_total_time="$seconds" -print_final_stats=1 \
       -artifact_prefix="$regressions/" "$BUILD/fuzz-work/$name" \
       >"$LOG_DIR/$name.log" 2>&1 &
    worker_pid=$!
    stop_worker() { kill -TERM -- "-$worker_pid" 2>/dev/null || kill -TERM "$worker_pid" 2>/dev/null || true; }
    trap stop_worker INT TERM HUP
    if wait "$worker_pid"; then rc=0; else rc=$?; fi
    end="$(python3 -c 'import time; print(time.monotonic())')"
    python3 - "$start" "$end" >"$LOG_DIR/$name.runtime" <<'PY'
import sys
print(max(0.0, float(sys.argv[2]) - float(sys.argv[1])))
PY
    exit "$rc") &
  pids+=("$!")
  pid_target["${pids[-1]}"]="$name"
  if (( ${#pids[@]} == 8 )); then wait_batch; fi
done
wait_batch
for name in "${names[@]}"; do verify_run "$name" || true; done
if (( campaign_failed )); then printf 'fuzz campaign incomplete; merge skipped\n' >&2; exit 1; fi

for name in "${names[@]}"; do
  (export TMPDIR="$BUILD/fuzz-tmp" QT_QPA_PLATFORM=offscreen
   set_active_sanitizers
   # -use_counters=0: keep the smallest set that covers every edge (hit-count buckets are not kept), so the checked-in corpus stays under the
   # cap; measured on the 10-minute campaign of 2026-10-17, edge coverage of the reduced corpora equalled or exceeded the full merge
   nice -n 10 "$FUZZ_DIR/fuzz_$name" -merge=1 -use_counters=0 "$ROOT/tests/fuzz/corpus/$name" "$BUILD/fuzz-work/$name") \
    >"$LOG_DIR/$name-merge.log" 2>&1
  if ! verify_corpus_cap "$ROOT/tests/fuzz/corpus/$name"; then corpus_cap_failed=1; fi
done
if (( corpus_cap_failed )); then
  printf 'fuzz merge produced one or more over-cap corpora; preserve full scratch results for review\n' >&2
  exit 1
fi
exit "$campaign_failed"
