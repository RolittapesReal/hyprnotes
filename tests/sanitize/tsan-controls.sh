#!/usr/bin/env bash
# Race controls for the two Qt-internal suppressions in tsan.supp. Needs a ThreadSanitizer-instrumented Qt 6.12 prefix (the stock Qt is
# not instrumented, so the Qt races are invisible there) and g++ with -fsanitize=thread.
#   tests/sanitize/tsan-controls.sh QT_PREFIX [ROUNDS]
# For each Qt-only reproducer: WITHOUT suppressions it must report the race (the report is real and Qt-internal), WITH tsan.supp it must
# report nothing. The Hyprnotes-style control (an int and a std::string raced by two threads) must still be reported WITH tsan.supp.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
P="${1:?usage: tsan-controls.sh QT_PREFIX [ROUNDS]}"; ROUNDS="${2:-150}"
W="$(mktemp -d "${TMPDIR:-/tmp}/hn-tsan-controls.XXXXXX")"; trap 'rm -rf "$W"' EXIT
MOC="${MOC:-$(command -v moc || ls "$P"/libexec/moc /usr/lib/qt6/moc 2>/dev/null | head -1)}"
INC="-I$P/include -I$P/include/QtCore -I$P/include/QtNetwork"
LIBS="-L$P/lib -lQt6Network -lQt6Core -Wl,-rpath,$P/lib"
export LD_LIBRARY_PATH="$P/lib" QT_PLUGIN_PATH="$P/plugins" QT_QPA_PLATFORM=offscreen
fail=0
ok() { echo "PASS $*"; }; bad() { echo "FAIL $*"; fail=1; }
build() { # name [moc]
  local src="$HERE/tsan_controls/$1.cpp"
  if [ -n "${2:-}" ]; then cp "$src" "$W/$1.cpp"; "$MOC" "$W/$1.cpp" -o "$W/$1.moc" || return 1; src="$W/$1.cpp"; fi
  g++ -std=c++20 -O1 -g -fsanitize=thread -fPIC -I"$W" "$src" -o "$W/$1" $INC $LIBS
}
races() { # binary args... ; prints the number of TSan reports, using SUPP (empty: none)
  local out; out="$("$@" 2>&1)"; echo "$out" | grep -c "WARNING: ThreadSanitizer"
}
base="halt_on_error=0:history_size=4"
SUPP="$HERE/tsan.supp"
# every run is repeated under the gate's own option (tests/sanitize/run.sh) and without it
for opt in "" ":ignore_interceptors_accesses=1"; do
  tag="${opt:+gate-options}"; tag="${tag:-plain}"
  build product_race || { echo "FAIL build product_race"; exit 1; }
  n=$(TSAN_OPTIONS="$base$opt:suppressions=$SUPP" races "$W/product_race")
  [ "${n:-0}" -ge 2 ] && ok "[$tag] product_race: $n reports with tsan.supp loaded" || bad "[$tag] product_race: only ${n:-0} reports with tsan.supp loaded"
  out="$(TSAN_OPTIONS="$base$opt:suppressions=$SUPP" "$W/product_race" 2>&1)"
  echo "$out" | grep -q "counter" && echo "$out" | grep -q "name" && ok "[$tag] product_race reports both the int and the std::string race" || bad "[$tag] product_race misses one of its two races"
  build app_lambda_race moc || { bad "build app_lambda_race"; continue; }
  n=$(TSAN_OPTIONS="$base$opt:suppressions=$SUPP" races "$W/app_lambda_race" 2000)
  [ "${n:-0}" -ge 1 ] && ok "[$tag] app_lambda_race (race under Qt frames): $n report(s) with tsan.supp loaded" || bad "[$tag] app_lambda_race hidden by tsan.supp"
  for c in qt_connection_race:moc qt_network_reply_race:; do
    name="${c%%:*}"; mocflag="${c#*:}"
    build "$name" "$mocflag" || { bad "build $name"; continue; }
    free=0; for _ in 1 2 3; do free=$(TSAN_OPTIONS="$base$opt" races "$W/$name" "$ROUNDS"); [ "$free" -ge 1 ] && break; done
    [ "$free" -ge 1 ] && ok "[$tag] $name: $free unsuppressed report(s), Qt-only code" || bad "[$tag] $name: no report without suppressions (control not positive)"
    got=""; sup=0
    for _ in 1 2 3; do
      log="$(TSAN_OPTIONS="$base$opt:suppressions=$SUPP:print_suppressions=1" "$W/$name" "$ROUNDS" 2>&1)"
      sup=$((sup + $(echo "$log" | grep -c "WARNING: ThreadSanitizer")))
      got="$got$log"
    done
    [ "$sup" -eq 0 ] && ok "[$tag] $name: 0 reports with tsan.supp (3 runs)" || bad "[$tag] $name: $sup report(s) survive tsan.supp"
    echo "$got" | grep -q "race:QObjectPrivate::maybeSignalConnected\|race:QHttpThreadDelegate" && ok "[$tag] $name: a tsan.supp entry matched" || bad "[$tag] $name: no tsan.supp entry matched (the race did not happen or the entry is dead)"
  done
done
exit $fail
