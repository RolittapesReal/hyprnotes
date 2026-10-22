#!/usr/bin/env bash
# Static analysis gate: GCC and Clang clean rebuilds must have 0 warnings; clang-tidy and cppcheck findings must all be
# listed in tests/static/accepted.txt ("<file>:<check> | <reason>", one per line; lines starting with # are comments).
# GCC runs on the host (build-stress); everything that needs clang/clang-tidy/cppcheck runs in the tools container.
#   tests/static/run.sh            run everything, exit 1 on any finding that is not accepted
#   HN_STATIC_STAGES="gcc clang tidy cppcheck"   run only some stages (default all)
#   HN_STATIC_SKIP_BUILD=1         do not rebuild; reuse the old gcc/clang build logs (refused if any source is newer than a log)
# Parsing and the accepted.txt rules live in check.py; tests/static/selftest.sh proves the parser flags what it must.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
TC="$ROOT/tests/tools/tools-container.sh"
OUT="${HN_STATIC_OUT:-$ROOT/build-static-out}"; mkdir -p "$OUT"
export HN_TOOLS_OUT="$OUT"
cd "$ROOT" || exit 2
rc=0

# Style noise, disabled on purpose (each of these fires ~20-50 times on code that is correct):
#  bugprone-easily-swappable-parameters  every (int, int) signature
#  bugprone-narrowing-conversions        qsizetype -> int on text offsets and row counts, bounded by QTextDocument's int positions (UI code);
#                                        enabled again for src/core and src/plugins in a second pass (NARROW_CHECKS below)
#  bugprone-implicit-widening-of-multiplication-result  constant unit maths (1024 * 1024) in int that fits
#  bugprone-signed-bitwise               Qt flag enums are signed
#  bugprone-switch-missing-default-case, bugprone-unhandled-code-paths  Qt role switches on int with a trailing return
#  bugprone-branch-clone                 parallel branches kept apart for readability (Markdown span kinds, status chain)
#  performance-enum-size                 enums are not stored in bulk
#  performance-use-std-move, performance-no-automatic-move   Qt value types are implicitly shared; a copy is a refcount bump
#  performance-unnecessary-copy-initialization   deliberate local copies (QPointer/QString captured by queued lambdas)
#  performance-unnecessary-value-param   std::function and implicitly shared sinks passed once per async call
#  clang-analyzer-optin.performance.Padding   per-plugin state struct, one instance per plugin
CHECKS='-*,bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions,-bugprone-implicit-widening-of-multiplication-result,-bugprone-signed-bitwise,-bugprone-switch-missing-default-case,-bugprone-unhandled-code-paths,-bugprone-branch-clone,performance-*,-performance-enum-size,-performance-use-std-move,-performance-no-automatic-move,-performance-unnecessary-copy-initialization,-performance-unnecessary-value-param,clang-analyzer-*,-clang-analyzer-optin.performance.Padding,concurrency-*,cert-err*,cert-str*,cert-flp*,misc-redundant-expression,misc-unused-*'

# cppcheck, disabled on purpose:
#  returnByReference, passedByValue  getters/callbacks use QString, QList and std::function, which are implicitly shared or sinks; a copy is a
#                                    refcount bump, and changing ~35 public signatures would widen dangling-reference risk for no measured gain
CPPCHECK_OFF='--suppress=returnByReference --suppress=passedByValue'

NARROW_CHECKS='-*,bugprone-narrowing-conversions'
CPPCHECK_OFF='--suppress=returnByReference --suppress=passedByValue'
# cppcheck, disabled on purpose:
#  returnByReference, passedByValue  getters/callbacks use QString, QList and std::function, which are implicitly shared or sinks; a copy is a
#                                    refcount bump, and changing ~35 public signatures would widen dangling-reference risk for no measured gain

STAGES="${HN_STATIC_STAGES:-gcc clang tidy cppcheck}"
want() { case " $STAGES " in *" $1 "*) return 0;; esac; return 1; }
fail() { echo "FAIL $*"; rc=1; }
check() { # NAME KIND LOG
  local out
  if [ ! -s "$3" ]; then fail "$1: log $3 is missing or empty"; return; fi
  out="$(python3 -I "$HERE/check.py" "$2" "$3" "$HERE/accepted.txt" "$ROOT")" && echo "ok   $1" || { fail "$1:"; echo "$out" | sed 's/^/  /'; }
}
# in-container tool run: logs are removed first, the command's own exit status and a completion marker are required
ctool() { # NAME LOG COMMAND (shell string, run in /work; stdout+stderr go to /out/LOG)
  local name="$1" log="$2" cmd="$3"
  rm -f "$OUT/$log"
  nice -n 10 "$TC" run clang -- bash -c "cd /work && { $cmd ; } > /out/$log 2>&1; echo \"HN_STATIC_DONE rc=\$?\" >> /out/$log" > "$OUT/$log.runner" 2>&1
  local trc=$?
  if [ $trc -ne 0 ]; then fail "$name: tools container did not run (rc=$trc): $(tail -2 "$OUT/$log.runner" | tr '\n' ' ')"; return 1; fi
  if ! tail -1 "$OUT/$log" 2>/dev/null | grep -q '^HN_STATIC_DONE rc=0$'; then fail "$name: tool did not finish cleanly ($(tail -1 "$OUT/$log" 2>/dev/null))"; return 1; fi
}

build_ok_log() { # LOG: build logs are written by this script; require the completion line
  tail -1 "$1" 2>/dev/null | grep -q '^HN_STATIC_DONE rc=0$'
}

"$HERE/selftest.sh" > "$OUT/selftest.log" 2>&1 || { fail "parser selftest (see $OUT/selftest.log)"; }

if want gcc || want clang; then
  if [ "${HN_STATIC_SKIP_BUILD:-0}" = 1 ]; then
    echo "WARNING: HN_STATIC_SKIP_BUILD=1: trusting OLD build logs in $OUT; this is not a gate run" >&2
    for t in gcc clang; do
      want $t || continue
      [ -s "$OUT/$t-build.log" ] || { fail "$t: no old build log to trust"; continue; }
      if [ -n "$(find src tests CMakeLists.txt -newer "$OUT/$t-build.log" -type f -print -quit)" ]; then fail "$t: sources are newer than $OUT/$t-build.log; rebuild"; fi
    done
  fi
fi

if [ "${HN_STATIC_SKIP_BUILD:-0}" != 1 ]; then
  if want gcc; then   # 1. GCC clean rebuild (host)
    rm -f "$OUT/gcc-build.log"
    { nice -n 10 cmake --build build-stress --clean-first -j8 2>&1; echo "HN_STATIC_DONE rc=$?"; } > "$OUT/gcc-build.log" 2>&1
    build_ok_log "$OUT/gcc-build.log" || fail "gcc build did not complete: $(tail -1 "$OUT/gcc-build.log")"
  fi
  if want clang; then   # 2. Clang clean rebuild (container)
    rm -f "$OUT/clang-build.log" "$OUT/clang-sync.log"
    nice -n 10 "$TC" build clang -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$OUT/clang-sync.log" 2>&1 || fail "clang configure/sync: $(tail -2 "$OUT/clang-sync.log" | tr '\n' ' ')"
    ctool clang-build clang-build.log 'ninja -C "$BUILD" clean >/dev/null && ninja -C "$BUILD" -j8'
  fi
fi
if want gcc; then build_ok_log "$OUT/gcc-build.log" || fail "gcc build log lacks successful completion marker"; fi
if want clang; then build_ok_log "$OUT/clang-build.log" || fail "clang build log lacks successful completion marker"; fi
want gcc && check "gcc: 0 warnings" compiler "$OUT/gcc-build.log"
want clang && check "clang: 0 warnings" compiler "$OUT/clang-build.log"

# 3. clang-tidy (container; compile_commands from the clang tree). Second pass: narrowing conversions for core and plugins only.
if want tidy; then
  ctool clang-tidy tidy.log "run-clang-tidy -p \"\$BUILD\" -j8 -checks='$CHECKS' '/work/src/.*\\.cpp'" && check clang-tidy tidy "$OUT/tidy.log"
  ctool clang-tidy-narrowing tidy-narrow.log "run-clang-tidy -p \"\$BUILD\" -j8 -checks='$NARROW_CHECKS' '/work/src/(core|plugins)/.*\\.cpp'" && check clang-tidy-narrowing tidy "$OUT/tidy-narrow.log"
fi

# 4. cppcheck (container). --library=qt knows Q_UNUSED/Q_DECLARE_METATYPE and friends.
if want cppcheck; then
  ctool cppcheck cppcheck.log "cppcheck --project=\"\$BUILD/compile_commands.json\" --library=qt --enable=warning,performance,portability --inline-suppr $CPPCHECK_OFF --suppress=missingIncludeSystem '--suppress=*:*/build/*' --verbose -j8" && check cppcheck cppcheck "$OUT/cppcheck.log"
fi
exit $rc
