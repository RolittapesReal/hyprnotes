#!/usr/bin/env bash
# Self-test: `clean --dry-run`, `--dry-run clean` must print actions and remove nothing; a stray argument is an error.
# Needs the image and both volumes to exist (run after `tools-container.sh image` and one build/run).
set -eu
H="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
snap() { { docker images -q hyprnotes-tools; docker volume ls -q --filter name=hyprnotes-tools; } | sort; }
before="$(snap)"; [ -n "$before" ] || { echo "nothing to protect: build the image first" >&2; exit 1; }
for a in "clean --dry-run" "--dry-run clean"; do
  out="$("$H/tools-container.sh" $a)"; echo "$out" | grep -q "would remove" || { echo "FAIL: no actions printed for: $a"; exit 1; }
  [ "$(snap)" = "$before" ] || { echo "FAIL: '$a' removed something"; exit 1; }
done
"$H/tools-container.sh" clean --dry-run bogus 2>/dev/null && { echo "FAIL: extra argument accepted"; exit 1; }
[ "$(snap)" = "$before" ] || { echo "FAIL: extra-arg run removed something"; exit 1; }
echo "PASS: dry-run is position independent and removes nothing"
