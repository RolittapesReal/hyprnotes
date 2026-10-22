#!/usr/bin/env bash
# Runs inside the tools container (tests/sanitize/run.sh heap starts it): valgrind massif over the in-process lifecycle stress
# (`stress_app cycles N`: open a sticky, type, close, over and over) and the growth check between two quiescent snapshots.
# stress_app's own memory checks read mallinfo2 and /proc, which valgrind replaces, so its exit status is ignored here.
#   HN_HEAP_CYCLES=N   extra cycles of the extended phase (default 1200)
set -u
cd "$BUILD"
export QT_QPA_PLATFORM=offscreen HOME=/tmp/hnhome XDG_RUNTIME_DIR=/tmp/hnrt QT_FORCE_STDERR_LOGGING=1
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
N="${HN_HEAP_CYCLES:-1200}"
rm -f /out/massif.out /out/massif-stress.log
valgrind --tool=massif --massif-out-file=/out/massif.out --time-unit=ms --max-snapshots=300 --detailed-freq=1 --threshold=0.05 --depth=14 \
  tests/stress/stress_app cycles "$N" > /out/massif-stress.log 2>&1
grep -E "widget/editor/sticky/session/thread/fd counts|widget count back" /out/massif-stress.log | cut -c1-160
ms_print /out/massif.out > /out/massif-print.txt
python3 /work/tests/sanitize/massif_diff.py /out/massif.out "$((N + 120))" 0.3
