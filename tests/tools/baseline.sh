#!/usr/bin/env bash
# Record a quality baseline into OUTDIR (rerunnable; writes only to OUTDIR and the build trees).
#   tests/tools/baseline.sh OUTDIR
# Needs build-stress (gcc, Release, -DHN_STRESS=ON); build-clang (clang, Release) is optional and skipped when clang is not installed.
# Produces: environment.txt, warnings-gcc.txt, warnings-clang.txt, ctest.txt, stress-quick.txt (+ stress-raw/ with the per-stage logs).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: baseline.sh OUTDIR}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
J="${JOBS:-8}"
cd "$ROOT" || exit 1

{ echo "date: $(date -Is)"; echo "kernel: $(uname -srvm)"; echo "cpu: $(grep -m1 'model name' /proc/cpuinfo)"; echo "cores: $(nproc)  load: $(cut -d' ' -f1-3 /proc/loadavg)"
  echo "gcc: $(gcc --version | head -1)"; echo "clang: $(clang --version 2>/dev/null | head -1 || true)"; command -v clang >/dev/null || echo "clang: not installed"
  echo "qt: $(pkg-config --modversion Qt6Core 2>/dev/null || echo unknown)"; echo "cmake: $(cmake --version | head -1)  ninja: $(ninja --version)"
  echo "head: $(git rev-parse --short HEAD 2>/dev/null || echo none)"; } > "$OUT/environment.txt"

# warnings: clean rebuild of one tree, keep every "warning:" line (deduplicated, path relative to the repo)
warnings() { # tree outfile
  local tree="$1" out="$2" log
  if [ ! -f "$ROOT/$tree/build.ninja" ]; then echo "tree $tree missing or not configured (compiler not installed?)" > "$out"; return; fi
  log="$ROOT/$tree/baseline-build.log"
  nice -n 10 cmake --build "$ROOT/$tree" --target clean > /dev/null 2>&1
  nice -n 10 cmake --build "$ROOT/$tree" -j"$J" > "$log" 2>&1
  { echo "# $(grep -c 'warning:' "$log") warning lines in $tree (clean rebuild, $(date -Is))"
    grep 'warning:' "$log" | sed "s|$ROOT/||" | sort -u; } > "$out"
}
warnings build-stress "$OUT/warnings-gcc.txt"
warnings build-clang  "$OUT/warnings-clang.txt"

# full ctest, serial, with per-test times
nice -n 10 ctest --test-dir "$ROOT/build-stress" -j1 --output-on-failure > "$OUT/ctest.txt" 2>&1

# quick stress run; the per-stage logs go to OUTDIR/stress-raw, the disk stage uses a folder inside the build tree
QUICK=1 BUILD_DIR="$ROOT/build-stress" OUT="$OUT/stress-raw" DISK_TMP="$ROOT/build-stress/stress-disk-tmp" \
  nice -n 10 "$ROOT/tests/stress/run_all.sh" > "$OUT/stress-quick.txt" 2>&1
cat "$OUT/stress-raw/index.txt" >> "$OUT/stress-quick.txt" 2>/dev/null
echo "baseline written to $OUT"
