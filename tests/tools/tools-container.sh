#!/usr/bin/env bash
# Docker tools container for tools the host lacks (clang, clang-tidy, cppcheck, valgrind, lcov, strace, hyperfine).
#
#   tools-container.sh [--dry-run] image                  build image hyprnotes-tools:latest (the only networked step)
#   tools-container.sh [--dry-run] build NAME [cmake args...]   sync source, configure+build /build/NAME (cmake -G Ninja, -j8)
#   tools-container.sh [--dry-run] run NAME -- CMD...     sync source, run CMD in /work with /build/NAME as $BUILD
#   tools-container.sh [--dry-run] clean                  remove hyprnotes-tools* image, containers, volumes (only those)
#
# Volumes: hyprnotes-tools-src (/work, copy of git-tracked + untracked-not-ignored files, mtimes kept so rebuilds
# are incremental), hyprnotes-tools-build (/build/<NAME>). Host dir $HN_TOOLS_OUT (default <scratchpad>/tools-out) is /out.
# Runs: --network none, --cpus 8, --memory 10g, no --privileged, nothing from the user's home.
# Note: the copied tree includes everything git lists, so large untracked dirs (build-*, baseline/) must be git-ignored
# or are excluded below.
set -eu
IMG="${HN_TOOLS_IMAGE:-hyprnotes-tools:latest}" SRC=hyprnotes-tools-src BLD=hyprnotes-tools-build
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${HN_TOOLS_OUT:-/tmp/claude-1000/-run-media-rohit-Arch-Coding-OPEN/aaa166ab-733e-409c-a5d7-70a0e1f545ec/scratchpad/tools-out}"
# --dry-run is honoured in any position (a destructive command must never run because the flag was misplaced)
DRY=0; ARGS=()
for a in "$@"; do if [ "$a" = "--dry-run" ]; then DRY=1; else ARGS+=("$a"); fi; done
set -- ${ARGS[@]+"${ARGS[@]}"}
case "$IMG" in hyprnotes-tools*) ;; *) echo "tools-container: HN_TOOLS_IMAGE must start with hyprnotes-tools" >&2; exit 2;; esac
command -v docker >/dev/null || { echo "tools-container: docker not found" >&2; exit 1; }
d() { if [ $DRY = 1 ]; then echo "+ docker $*"; else docker "$@"; fi; }
RUNOPTS=(--rm --network none --cpus 8 --memory 10g -v "$SRC:/work" -v "$BLD:/build" -v "$OUT:/out")

sync_src() {
  mkdir -p "$OUT"
  if [ $DRY = 1 ]; then echo "+ sync: git ls-files | tar | docker run ... tar -x (delete files not in list first)"; return; fi
  # exact copy: list of wanted files goes in, anything else in /work (except nothing special) is deleted first
  (cd "$ROOT" && git ls-files -z --cached --others --exclude-standard | while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done \
     | grep -zv -e '^build-' -e '^baseline/' -e '^\.superpowers/' > "$OUT/.files.z")
  docker volume create "$SRC" >/dev/null; docker volume create "$BLD" >/dev/null
  # drop files no longer wanted, then unpack (tar keeps mtimes, so unchanged files stay unchanged)
  docker run --rm -i -v "$SRC:/work" "$IMG" bash -c \
    'cat > /tmp/want.z; cd /work; find . -type f -print0 | sed -z "s|^\./||" | sort -z > /tmp/have.z; sort -z /tmp/want.z > /tmp/want.s
     comm -z -23 /tmp/have.z /tmp/want.s | xargs -0 -r rm -f' < "$OUT/.files.z"
  (cd "$ROOT" && tar --null -T "$OUT/.files.z" -cf -) | docker run --rm -i -v "$SRC:/work" "$IMG" bash -c 'tar -x -C /work'
}

cmd="${1:-}"; [ -n "$cmd" ] && shift
case "$cmd" in
  image) [ $# = 0 ] || { echo "tools-container: image takes no arguments: $*" >&2; exit 2; }
    d build -t "$IMG" -f "$HERE/tools.Dockerfile" "$HERE" ;;
  build)
    name="${1:?NAME}"; shift
    sync_src
    d run "${RUNOPTS[@]}" "$IMG" bash -c 'n=$1; shift; cmake -S /work -B /build/$n -G Ninja "$@" && cmake --build /build/$n -j8' _ "$name" "$@" ;;
  run)
    name="${1:?NAME}"; shift; [ "${1:-}" = "--" ] && shift
    sync_src
    d run "${RUNOPTS[@]}" -w /work -e BUILD="/build/$name" "$IMG" "$@" ;;
  clean)
    [ $# = 0 ] || { echo "tools-container: clean takes no arguments: $*" >&2; exit 2; }
    if [ $DRY = 1 ]; then
      echo "would remove containers:"; docker ps -a --filter name=hyprnotes-tools --format '  {{.Names}}'
      echo "would remove volumes:"; docker volume ls --filter name=hyprnotes-tools --format '  {{.Name}}'
      echo "would remove image:"; docker images --filter reference='hyprnotes-tools*' --format '  {{.Repository}}:{{.Tag}}'
    else
      docker ps -aq --filter name=hyprnotes-tools | xargs -r docker rm -f
      docker volume ls -q --filter name=hyprnotes-tools | xargs -r docker volume rm
      docker images -q --filter reference='hyprnotes-tools*' | xargs -r docker rmi
    fi ;;
  *) sed -n '2,8p' "${BASH_SOURCE[0]}"; exit 2 ;;
esac
