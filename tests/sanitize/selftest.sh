#!/usr/bin/env bash
# Proves the sanitizer gate does not pass when nothing ran. Host part: ctest with a regex that matches nothing must fail, and run.sh's
# `need` helper must reject a low count. Container part (only if the tools image exists): the valgrind script with a wrong regex must fail.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; cd "$ROOT" || exit 2
fail=0
ok() { echo "PASS $*"; }; bad() { echo "FAIL $*"; fail=1; }
ctest --test-dir build-asan -R '^no_such_test_xyz$' --no-tests=error > /dev/null 2>&1 && bad "ctest --no-tests=error accepted an empty selection" || ok "ctest rejects an empty selection"
eval "$(sed -n '/^need() {/,/^}/p' "$HERE/run.sh")"
need t 0 1 > /dev/null && bad "need accepted 0 of 1" || ok "need rejects a short count"
need t 5 1 > /dev/null && ok "need accepts an enough count" || bad "need rejected a good count"
if docker image inspect hyprnotes-tools:latest > /dev/null 2>&1; then
  export HN_TOOLS_OUT="${HN_SANITIZE_OUT:-$ROOT/build-sanitize-out}"; mkdir -p "$HN_TOOLS_OUT"
  out=$(tests/tools/tools-container.sh run valgrind -- bash /work/tests/sanitize/valgrind-in-container.sh '^no_such_test_xyz$' 2>&1); rc=$?
  [ $rc != 0 ] && echo "$out" | grep -q HN_VALGRIND_FAIL && ok "valgrind script rejects a wrong regex (rc=$rc)" || bad "valgrind script passed with a wrong regex (rc=$rc): $out"
fi
exit $fail
