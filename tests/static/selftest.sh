#!/usr/bin/env bash
# Proves check.py flags what it must, using fabricated logs (no tools needed). Run: tests/static/selftest.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT=/repo/root
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
printf '# empty\nsrc/a.cpp:bugprone-x | reason\n' > "$T/acc"
fails=0
expect() { # NAME WANT(0|1) KIND LOG-TEXT
  printf '%s\n' "$4" > "$T/log"
  python3 -I "$HERE/check.py" "$3" "$T/log" "$T/acc" "$ROOT" > "$T/out"; local rc=$?
  if [ "$rc" = "$2" ]; then echo "ok   $1"; else echo "FAIL $1 (rc=$rc, wanted $2)"; cat "$T/out"; fails=1; fi
}
expect "tidy: unaccepted finding flagged"      1 tidy     $'Processing /work/src/b.cpp\n/work/src/b.cpp:3:4: warning: bad [bugprone-x]'
expect "tidy: accepted finding passes"         0 tidy     $'Processing /work/src/a.cpp\n/work/src/a.cpp:3:4: warning: bad [bugprone-x]'
expect "tidy: second site of accepted check"   1 tidy     $'Processing /work/src/a.cpp\n/work/src/a.cpp:3:4: warning: bad [bugprone-x]\n/work/src/a.cpp:9:4: warning: bad [bugprone-x]'
expect "tidy: same site from two TUs is one"   0 tidy     $'Processing /work/src/a.cpp\n/work/src/a.cpp:3:4: warning: bad [bugprone-x]\n/work/src/a.cpp:3:4: warning: bad [bugprone-x]'
expect "tidy: wrapped diagnostic without id"   1 tidy     $'/work/src/a.cpp:3:4: warning: this message was wrapped\n  and has no check name'
expect "cppcheck: id-less error flagged"       1 cppcheck "/work/src/c.cpp:1:1: error: Bailing out from checking"
expect "cppcheck: id'd performance flagged"    1 cppcheck "/work/src/c.h:1:1: performance: Function should return by ref. [returnByReference]"
expect "cppcheck: clean log passes"            0 cppcheck $'Checking /work/src/a.cpp...\nHN_STATIC_DONE rc=0'
expect "gcc: absolute-path warning flagged"    1 compiler "$ROOT/tests/unit/x_test.cpp:7:9: warning: unused variable 'v' [-Wunused-variable]"
expect "gcc: generated-file warning flagged"   1 compiler "$ROOT/build-stress/src/moc_x.cpp:7:9: warning: oops [-Wfoo]"
expect "gcc: /work path warning flagged"       1 compiler "/work/src/a.cpp:7:9: warning: oops [-Wfoo]"
expect "gcc: in-repo warning never acceptable" 1 compiler "/work/src/a.cpp:3:4: warning: oops [bugprone-x]"
expect "gcc: no-flag warning flagged"          1 compiler "$ROOT/src/a.cpp:7:9: warning: unknown escape sequence: '\\@'"
expect "gcc: clean log passes"                 0 compiler "[1/2] Building CXX object a.o"
# A completion marker is status metadata, not evidence that analysis processed source.
expect "tidy: marker-only log rejected"        1 tidy     "HN_STATIC_DONE rc=0"
expect "tidy: empty analysis rejected"          1 tidy     ""
expect "tidy: processed source with no findings passes" 0 tidy $'Processing /work/src/a.cpp\nHN_STATIC_DONE rc=0'
expect "tidy: current run-clang-tidy command proves processed source" 0 tidy $'[1/1] /usr/sbin/clang-tidy -checks=-*,bugprone-* -p=/build/clang /work/src/a.cpp\nHN_STATIC_DONE rc=0'
expect "tidy: uppercase source suffix recognized" 0 tidy $'Processing file /work/src/a.CPP.\nHN_STATIC_DONE rc=0'
expect "cppcheck: marker-only log rejected"    1 cppcheck "HN_STATIC_DONE rc=0"
expect "cppcheck: processed source with no findings passes" 0 cppcheck $'Checking /work/src/a.cpp...\nHN_STATIC_DONE rc=0'
expect "cppcheck: verbose checking line with spaced ellipsis passes" 0 cppcheck $'Checking /work/src/a.CPP ...\nHN_STATIC_DONE rc=0'
expect "gcc: marker-only log rejected"         1 compiler "HN_STATIC_DONE rc=0"
expect "gcc: compilation unit passes"          0 compiler "[1/2] Building CXX object a.o"
exit $fails
