#!/usr/bin/env bash
# Runs inside the tools container (tests/sanitize/run.sh valgrind starts it): valgrind memcheck over the core, editor, theme, mods and
# plugin test executables of the container build tree $BUILD (a RelWithDebInfo tree named "valgrind", built by run.sh).
# GUI app tests are not run here (they are covered by ASan/LSan and TSan on the host, and valgrind makes a widget test 50x slower).
#   valgrind-in-container.sh [REGEX]   ctest -R regex of the tests to run (default below). Fewer than MIN tests selected is a failure (a wrong
#   regex or an unbuilt tree must not pass silently); MIN is 30 for the default set, 1 when a regex is given.
set -u
cd "$BUILD"
export QT_QPA_PLATFORM=offscreen HOME=/tmp/hnhome XDG_RUNTIME_DIR=/tmp/hnrt
export HN_RECOVERY_SAVES=6000   # core_recovery: 100k saves take > 5 min under valgrind (QtTest watchdog); 6000 still exceeds the 4096-entry cap
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
RE="${1:-^(core_|editor_|theme_|mods_|plugins_)}"; MIN=30; [ -n "${1:-}" ] && MIN=1
TESTS=$(ctest -N -R "$RE" | sed -n 's/^ *Test *#[0-9]*: //p')
n=$(printf '%s\n' "$TESTS" | grep -c .)
if [ "$n" -lt "$MIN" ]; then echo "HN_VALGRIND_FAIL: only $n tests match '$RE' in $BUILD (need at least $MIN): tree not built or regex wrong"; exit 3; fi
ran=0
gcc -shared -fPIC -o /tmp/nojit.so /work/tests/sanitize/nojit.c || exit 2   # see nojit.c: PCRE2 JIT code cannot be checked by memcheck
SUPP=/work/tests/sanitize/valgrind.supp
supp=(); [ -s "$SUPP" ] && supp=(--suppressions="$SUPP")
bad=0
# Tests with wall-clock assertions (p95 budgets, 2 s sandbox budgets, perf targets) cannot pass at valgrind's 20-50x slowdown. Their
# functional failures are expected here; what the run judges for them is the memcheck error count, like for every other test.
# No --error-exitcode: ModHost probes each mod by dlopen in a forked child (D12), and in that child valgrind sees the parent's objects
# whose only references are on other threads' stacks as "definitely lost", which would make the probe exit non-zero and the mod "not
# loadable". --child-silent-after-fork hides the child; the parent's own ERROR SUMMARY (leaks of kinds definite,indirect count as errors) is the gate.
TIMING=" core_index core_graph editor_links_test editor_perf_test editor_realnotes_test plugins_sandbox plugins_hardening plugins_examples plugins_v2 "
for t in $TESTS; do
  exe=$(ctest -N -V -R "^$t\$" | sed -n 's/.*Test command: //p' | head -1)
  if [ -z "$exe" ] || [ ! -x "${exe%% *}" ]; then bad=$((bad + 1)); echo "$t: test binary missing ($exe)"; continue; fi
  ran=$((ran + 1))
  log=/out/valgrind-$t.log
  start=$(date +%s)
  LD_PRELOAD=/tmp/nojit.so timeout 3000 valgrind --child-silent-after-fork=yes --leak-check=full --errors-for-leak-kinds=definite,indirect --show-leak-kinds=definite,indirect \
    --track-origins=no --num-callers=30 "${supp[@]}" $exe > "$log" 2>&1
  rc=$?
  errs=$(sed -n 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/p' "$log" | tail -1)
  fails=$(grep -c '^FAIL!' "$log")
  printf '%-28s rc=%-3s valgrind-errors=%-4s test-failures=%-3s %ss\n' "$t" "$rc" "${errs:-?}" "$fails" "$(( $(date +%s) - start ))"
  if [ "${errs:-?}" != 0 ]; then bad=$((bad + 1)); echo "  ^ memcheck errors (or no summary): see $log"
  elif [ "$fails" != 0 ]; then case "$TIMING" in *" $t "*) ;; *) bad=$((bad + 1)); echo "  ^ unexpected test failure under valgrind: see $log";; esac; fi
done
[ "$ran" -ge "$MIN" ] || { echo "HN_VALGRIND_FAIL: ran only $ran tests (need $MIN)"; exit 3; }
echo "HN_VALGRIND_DONE ran=$ran bad=$bad"
[ "$bad" = 0 ]
