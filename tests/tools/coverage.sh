#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
rm -f build-cov/coverage.json build-cov/coverage-gcovr.json build-cov/coverage.html
grep -q '^HN_COVERAGE:BOOL=ON$' build-cov/CMakeCache.txt
nice -n 10 cmake --build build-cov -j8
find build-cov -name '*.gcda' -delete
scratch=$(mktemp -d /tmp/hyprnotes-coverage.XXXXXX)
trap 'rm -rf "$scratch"' EXIT
export HOME="$scratch/home" XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data"
export XDG_STATE_HOME="$scratch/state" XDG_CACHE_HOME="$scratch/cache"
export XDG_RUNTIME_DIR="$scratch/runtime"
export HN_CONFIG_DIR="$scratch/hn-config" HN_DATA_DIR="$scratch/hn-data"
export HN_STATE_DIR="$scratch/hn-state" HN_CACHE_DIR="$scratch/hn-cache"
export HN_NOTES_DIR="$HOME/Notes/Hyprnotes"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR" "$HN_CONFIG_DIR" "$HN_DATA_DIR" "$HN_STATE_DIR" "$HN_CACHE_DIR" "$HN_NOTES_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM="${HN_COVERAGE_QPA_PLATFORM:-offscreen}"
status=0
nice -n 10 ctest --test-dir build-cov -j1 --timeout 900 --no-tests=error --output-on-failure || status=1
if (( status )); then
  echo 'Tests failed: the following coverage report is diagnostic; the gate remains failed.' >&2
fi
nice -n 10 python3 tests/tools/coverage.py || status=1
exit "$status"
