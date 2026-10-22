#!/usr/bin/env bash
# Run after building fuzz_regressions: tests fixture-root replay failure modes.
set -euo pipefail
BIN="${1:-}"
if [[ -z "$BIN" || ! -x "$BIN" ]]; then
  printf 'usage: %s /path/to/fuzz_regressions\n' "$0" >&2
  exit 2
fi
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1
mkdir -p "$T/empty" "$T/invalid/markdown_roundtrip" "$T/valid/markdown_roundtrip"

if "$BIN" --root "$T/empty" >"$T/empty.out" 2>&1; then
  printf 'FAIL empty fixture root accepted\n'; exit 1
fi
grep -q 'replayed 0 files' "$T/empty.out"
printf 'ok   zero-replay-rejected\n'

printf '*invalid*' > "$T/invalid/markdown_roundtrip/bad.base64"
if "$BIN" --root "$T/invalid" >"$T/invalid.out" 2>&1; then
  printf 'FAIL malformed base64 accepted\n'; exit 1
fi
grep -q 'invalid or empty base64' "$T/invalid.out"
grep -q 'replayed 0 files' "$T/invalid.out"
printf 'ok   malformed-base64-rejected\n'

printf 'x' > "$T/valid/markdown_roundtrip/one.bin"
printf 'eA==\n' > "$T/valid/markdown_roundtrip/one.base64"
"$BIN" --root "$T/valid" >"$T/valid.out" 2>&1
grep -q 'replayed 2 files, 0 failures' "$T/valid.out"
printf 'ok   one-fixture-replayed-and-counted\n'

ln -s "$T/no-such-file" "$T/invalid/markdown_roundtrip/unreadable.bin"
if "$BIN" --root "$T/invalid" >"$T/unreadable.out" 2>&1; then
  printf 'FAIL unreadable fixture accepted\n'; exit 1
fi
grep -q 'regression is not a regular file' "$T/unreadable.out"
printf 'ok   symlink-fixture-rejected\n'
