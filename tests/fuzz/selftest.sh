#!/usr/bin/env bash
# Exercises fuzz runner gates with 13 local stubs; never launches libFuzzer.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
python3 "$HERE/curation_selftest.py"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
ENV_RECORD="$T/sanitizer-env.log"
export HN_FUZZ_STUB_ENV_RECORD="$ENV_RECORD"
names=(markdown_roundtrip frontmatter links query_spec fts_query manifest plugin_archive theme_import config ipc_transport hypr_events session market_index)
BUILD="$T/build"; mkdir -p "$BUILD/tests/fuzz" "$T/root/tests/fuzz/corpus"
for name in "${names[@]}"; do
  mkdir -p "$T/root/tests/fuzz/corpus/$name"
  printf seed > "$T/root/tests/fuzz/corpus/$name/seed"
  cat > "$BUILD/tests/fuzz/fuzz_$name" <<'STUB'
#!/usr/bin/env bash
stub_name="$(basename "$0")"
stub_phase=campaign
[[ " $* " == *" -merge=1 "* ]] && stub_phase=merge
if ! { [[ "${HN_FUZZ_STUB_MODE:-}" == omit-campaign-record && "$stub_name" == fuzz_links && "$stub_phase" == campaign ]] ||
       [[ "${HN_FUZZ_STUB_MODE:-}" == omit-merge-record && "$stub_name" == fuzz_links && "$stub_phase" == merge ]]; }; then
  printf '%s|%s|%s|%s|%s|%s\n' "$stub_name" "${ASAN_OPTIONS-}" "${LSAN_OPTIONS-}" "${UBSAN_OPTIONS-}" "$stub_phase" "$*" >> "$HN_FUZZ_STUB_ENV_RECORD"
fi
case "${HN_FUZZ_STUB_MODE:-valid}" in
  crash) exit 77 ;;
  missing) sleep "${HN_FUZZ_STUB_SLEEP:-0.1}"; exit 0 ;;
  zero) sleep "${HN_FUZZ_STUB_SLEEP:-0.1}"; printf '#0 INITED cov: 0 ft: 0\nstat::number_of_executed_units: 0\n'; exit 0 ;;
  short) sleep 0.05 ;;
  *) sleep "${HN_FUZZ_STUB_SLEEP:-1}" ;;
esac
if [[ "${HN_FUZZ_STUB_MODE:-valid}" == merge-fail ]]; then
  for arg in "$@"; do [[ "$arg" != -merge=1 ]] || exit 66; done
fi
if [[ "${HN_FUZZ_STUB_MODE:-valid}" == merge-overcap && "$(basename "$0")" == fuzz_markdown_roundtrip ]]; then
  previous=
  for arg in "$@"; do
    if [[ "$previous" == -merge=1 && "$arg" != -* ]]; then
      for i in $(seq 1 200); do printf x > "$arg/overcap-$i"; done
      break
    fi
    [[ "$arg" == -merge=1 ]] && previous="$arg"
  done
fi
printf '#1 INITED cov: 1 ft: 1\nstat::number_of_executed_units: 1\n'
STUB
  chmod +x "$BUILD/tests/fuzz/fuzz_$name"
done

assert_active_sanitizers() {
  local record binary asan lsan ubsan phase args key last
  declare -A seen=()
  while IFS='|' read -r binary asan lsan ubsan phase args; do
    [[ -n "$binary" && -n "$phase" ]] || continue
    [[ -z "${seen[$binary|$phase]+x}" ]] || { echo "FAIL duplicate sanitizer record $binary $phase"; return 1; }
    seen[$binary|$phase]=1
    for key in alloc_dealloc_mismatch detect_leaks strict_string_checks detect_stack_use_after_return; do
      last="$(tr ':' '\n' <<< "$asan" | awk -F= -v k="$key" '$1==k {v=$2} END {print v}')"
      [[ "$last" == 1 ]] || { echo "FAIL $binary effective ASan $key=$last: $asan"; return 1; }
    done
    [[ "$lsan" != *suppressions=* ]] || { echo "FAIL $binary injected LSan suppression: $lsan"; return 1; }
    last="$(tr ':' '\n' <<< "$lsan" | awk -F= '$1=="detect_leaks" {v=$2} END {print v}')"
    [[ "$last" == 1 ]] || { echo "FAIL $binary effective LSan detect_leaks=$last: $lsan"; return 1; }
    last="$(tr ':' '\n' <<< "$ubsan" | awk -F= '$1=="halt_on_error" {v=$2} END {print v}')"
    [[ "$last" == 1 ]] || { echo "FAIL $binary effective UBSan halt_on_error=$last: $ubsan"; return 1; }
  done < "$ENV_RECORD"
  [[ -s "$ENV_RECORD" ]] || { echo 'FAIL no child sanitizer environments recorded'; return 1; }
  for binary in "${names[@]}"; do
    for phase in campaign merge; do
      [[ "${seen[fuzz_$binary|$phase]+x}" ]] || { echo "FAIL fuzz_$binary $phase expected exactly 1 sanitizer record, got 0"; return 1; }
    done
  done
  ((${#seen[@]} == ${#names[@]} * 2)) || { echo "FAIL expected exactly $(( ${#names[@]} * 2 )) target-phase records, got ${#seen[@]}"; return 1; }
}

expect_suppression_rejected() {
  local option="$1" value="$2" label="$3"
  : > "$ENV_RECORD"
  if env BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$option=$value" "$HERE/run.sh" 1 >"$T/$label.out" 2>&1; then
    echo "FAIL $label: suppression/weakening option accepted"; return 1
  fi
  grep -qi 'strict fuzz mode rejects' "$T/$label.out" || { cat "$T/$label.out"; echo "FAIL $label: missing strict rejection"; return 1; }
  [[ ! -s "$ENV_RECORD" ]] || { echo "FAIL $label: child launched before rejection"; return 1; }
}
expect_suppression_rejected ASAN_OPTIONS 'suppressions=/tmp/asan.supp' reject-asan-suppression
expect_suppression_rejected LSAN_OPTIONS 'suppressions=/tmp/lsan.supp' reject-lsan-suppression
expect_suppression_rejected LSAN_OPTIONS 'use_tls=0' reject-lsan-ignore-option
expect_suppression_rejected UBSAN_OPTIONS 'suppressions=/tmp/ubsan.supp' reject-ubsan-suppression
expect_suppression_rejected ASAN_OPTIONS 'exitcode=0' reject-asan-exitcode
expect_suppression_rejected LSAN_OPTIONS 'exitcode=0' reject-lsan-exitcode
expect_suppression_rejected UBSAN_OPTIONS 'exitcode=0' reject-ubsan-exitcode
expect_suppression_rejected ASAN_OPTIONS 'alloc_dealloc_mismatch=0' reject-asan-disable
expect_suppression_rejected LSAN_OPTIONS 'detect_leaks=0' reject-lsan-disable
expect_suppression_rejected UBSAN_OPTIONS 'halt_on_error=0' reject-ubsan-disable
printf 'ok   sanitizer-suppressions-and-weakening-rejected-preflight\n'

: > "$ENV_RECORD"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 HN_FUZZ_STUB_MODE=valid \
   HN_FUZZ_STUB_SLEEP=0.02 ASAN_OPTIONS='print_stats=1' LSAN_OPTIONS='verbosity=0' UBSAN_OPTIONS='print_stacktrace=1' \
   "$HERE/run.sh" 1 >"$T/sanitizer-green.out" 2>&1; then :; else cat "$T/sanitizer-green.out"; exit 1; fi
assert_active_sanitizers
grep -q 'print_stats=1' "$ENV_RECORD"
grep -q 'verbosity=0' "$ENV_RECORD"
grep -q 'print_stacktrace=1' "$ENV_RECORD"
printf 'ok   sanitizer-benign-caller-options-preserved\n'

for omitted_phase in campaign merge; do
  : > "$ENV_RECORD"
  BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 HN_FUZZ_STUB_MODE="omit-$omitted_phase-record" \
    HN_FUZZ_STUB_SLEEP=0.02 "$HERE/run.sh" 1 >"$T/omitted-$omitted_phase.out" 2>&1
  if assert_active_sanitizers >"$T/omitted-$omitted_phase-assert.out" 2>&1; then
    echo "FAIL omitted $omitted_phase target-phase environment record accepted"; exit 1
  fi
  grep -q "fuzz_links $omitted_phase expected exactly 1" "$T/omitted-$omitted_phase-assert.out"
  printf 'ok   missing-%s-target-phase-record-rejected\n' "$omitted_phase"
done

expect_fail() {
  local label="$1" mode="$2" expected="$3"
  if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.2 HN_FUZZ_STUB_MODE="$mode" \
     HN_FUZZ_STUB_SLEEP=0.25 "$HERE/run.sh" 1 >"$T/$label.out" 2>&1; then
    printf 'FAIL %s: runner unexpectedly passed\n' "$label"; return 1
  fi
  if ! grep -q "$expected" "$T/$label.out"; then
    printf 'FAIL %s: expected evidence %s missing\n' "$label" "$expected"
    cat "$T/$label.out"
    return 1
  fi
  printf 'ok   %s\n' "$label"
}
expect_fail zero-units zero 'units/cov/ft'
expect_fail missing-stats missing 'units/cov/ft'
expect_fail crash crash 'incomplete'
expect_fail early-exit short 'ran .* requested'

BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.2 HN_FUZZ_STUB_MODE=valid \
  HN_FUZZ_STUB_SLEEP=0.25 "$HERE/run.sh" 1 >"$T/valid.out" 2>&1
printf 'ok   valid-duration-stats\n'

if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.2 HN_FUZZ_STUB_MODE=merge-fail \
   HN_FUZZ_STUB_SLEEP=0.25 "$HERE/run.sh" 1 >"$T/merge-fail.out" 2>&1; then
  printf 'FAIL failed merge accepted\n'; exit 1
fi
[[ -e "$BUILD/fuzz-logs/markdown_roundtrip-merge.log" ]]
printf 'ok   failed-merge-propagated\n'

rm "$BUILD/tests/fuzz/fuzz_session"
rm -rf "$BUILD/fuzz-logs"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$HERE/run.sh" 1 >"$T/preflight.out" 2>&1; then
  printf 'FAIL preflight-missing-binary: runner unexpectedly passed\n'; exit 1
fi
grep -q 'missing fuzz binary:.*fuzz_session' "$T/preflight.out"
if compgen -G "$BUILD/fuzz-logs/*.log" >/dev/null; then
  printf 'FAIL preflight launched targets before checking all binaries\n'; exit 1
fi
printf 'ok   preflight-all-targets-before-launch\n'

cp "$BUILD/tests/fuzz/fuzz_markdown_roundtrip" "$BUILD/tests/fuzz/fuzz_session"
mv "$T/root/tests/fuzz/corpus/market_index" "$T/market_index-corpus"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$HERE/run.sh" 1 >"$T/missing-corpus.out" 2>&1; then
  printf 'FAIL preflight-missing-corpus: runner unexpectedly passed\n'; exit 1
fi
grep -q 'missing corpus:.*market_index' "$T/missing-corpus.out"
mv "$T/market_index-corpus" "$T/root/tests/fuzz/corpus/market_index"
printf 'ok   preflight-all-corpora\n'

mkdir -p "$T/root/tests/fuzz/regressions/markdown_roundtrip"
printf 'eA==\n' > "$T/root/tests/fuzz/regressions/markdown_roundtrip/retained.base64"
for i in $(seq 1 198); do printf x > "$T/root/tests/fuzz/corpus/config/overcap-$i"; done
rm -rf "$BUILD/fuzz-logs"
if ! BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$HERE/run.sh" 1 >"$T/count-199.out" 2>&1; then
  cat "$T/count-199.out"
  printf 'FAIL under-200 corpus count rejected\n'; exit 1
fi
printf 'ok   corpus-count-199-accepted\n'
printf x > "$T/root/tests/fuzz/corpus/config/overcap-199"
rm -rf "$BUILD/fuzz-logs"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$HERE/run.sh" 1 >"$T/count-cap.out" 2>&1; then
  printf 'FAIL over-cap corpus count accepted\n'; exit 1
fi
grep -q 'FAIL corpus cap:.*200 files' "$T/count-cap.out"
[[ -f "$T/root/tests/fuzz/regressions/markdown_roundtrip/retained.base64" ]]
if compgen -G "$BUILD/fuzz-logs/*.log" >/dev/null; then
  printf 'FAIL count-cap preflight launched targets\n'; exit 1
fi
printf 'ok   corpus-count-cap-fails-closed-regressions-independent\n'

rm -f "$T/root/tests/fuzz/corpus/config"/overcap-*
python3 - "$T/root/tests/fuzz/corpus/config/overcap-bytes" <<'PY'
import pathlib, sys
pathlib.Path(sys.argv[1]).write_bytes(b"x" * (1024 * 1024))
PY
rm -rf "$BUILD/fuzz-logs"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 "$HERE/run.sh" 1 >"$T/byte-cap.out" 2>&1; then
  printf 'FAIL over-cap corpus bytes accepted\n'; exit 1
fi
grep -q 'FAIL corpus cap:.*bytes' "$T/byte-cap.out"
if compgen -G "$BUILD/fuzz-logs/*.log" >/dev/null; then
  printf 'FAIL byte-cap preflight launched targets\n'; exit 1
fi
printf 'ok   corpus-byte-cap-fails-closed\n'

rm -f "$T/root/tests/fuzz/corpus/config/overcap-bytes"
rm -rf "$T/root/tests/fuzz/regressions"
mkdir -p "$T/root/tests/fuzz/regressions/markdown_roundtrip"
printf 'eA==\n' > "$T/root/tests/fuzz/regressions/markdown_roundtrip/retained.base64"
rm -rf "$BUILD/fuzz-logs" "$BUILD/fuzz-work"
if BUILD="$BUILD" HN_FUZZ_SELFTEST=1 HN_FUZZ_TEST_ROOT="$T/root" HN_FUZZ_TEST_SECONDS=0.01 HN_FUZZ_STUB_MODE=merge-overcap \
   HN_FUZZ_STUB_SLEEP=0.02 "$HERE/run.sh" 1 >"$T/post-merge-cap.out" 2>&1; then
  printf 'FAIL over-cap merge accepted\n'; exit 1
fi
if ! grep -q 'FAIL corpus cap:.*201 files' "$T/post-merge-cap.out" || ! grep -q 'preserve full scratch results' "$T/post-merge-cap.out"; then
  cat "$T/post-merge-cap.out"
  printf 'FAIL post-merge cap did not report expected failure\n'; exit 1
fi
[[ -f "$T/root/tests/fuzz/regressions/markdown_roundtrip/retained.base64" ]]
printf 'ok   post-merge-cap-fails-closed-regression-retained\n'
