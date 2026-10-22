# Testing and validation

**Status note (2026-10-22):** The scoped Part A functional closeout is complete. It covers Markdown list save/reopen behavior, Organizer pagination during index sync, focused regressions, and a bounded allocator replay. The broader hardening qualification is still deferred. Recorded performance and memory budget failures remain failures; the smaller closeout scope did not change them.

## 0.2.2 UI update

The October 22 native Release build reports `hyprnotes 0.2.2`. Selected checks across eight UI test executables passed, covering swatch fills, tooltips, note and settings transitions, sticky layout, filter clearing, the shared tray icon and Keys navigation. Five CLI/plugin executables also passed their selected version and binding checks. These runs recorded 79 QtTest entries passed, none failed or skipped; that count includes each executable’s setup and cleanup entries.

The screenshot suite’s `states` and `screenshots` cases passed with private app data and refreshed the light and dark images used in these docs. All checks used isolated HOME, XDG and Hyprnotes storage. They exercised Qt’s offscreen platform, not the running desktop app.

This was a focused UI and release-metadata check. The full suite, sanitizers and compositor matrix were not rerun for 0.2.2. The earlier results and limits below remain unchanged.

## Recorded Part A evidence

- **0.2.1 release metadata:** the app and five affected CLI/plugin test executables rebuilt successfully. The app reports `hyprnotes 0.2.1`. Three runtime suites passed on the first run; two app suites initially failed because the sandbox blocked their local test sockets, then both passed with local socket access and private test data. No assertion or sanitizer check was changed to obtain these passes.
- **Focused regressions:** the final isolated offscreen run passed `editor_realnotes_test`, `editor_roundtrip_test`, and `app_ui_test` (3/3 CTest targets; 219 QtTest cases passed, none failed or skipped).
- **Release:** before the 0.2.1 version update, the full CTest run passed 88/88 in 155.98 seconds. This is executable-level accounting; optional cases inside some QtTest suites were skipped. Its 1,750-entry source manifest was verified against that snapshot.
- **ASan/LSan/UBSan:** the last full run before the 0.2.1 version update passed 87/88 in 250.71 seconds. The remaining failure was the unchanged mixed-20-KiB load assertion: 60.27 ms against 30 ms. Allocator-mismatch, leak, strict-string, stack-use-after-return and UBSan halt-on-error checks were active; no sanitizer diagnostic appeared. This remains a failed aggregate run with a deferred numeric timing failure.
- **ThreadSanitizer:** an earlier full run completed 85/88 under the reviewed suppression file, with four numeric timing assertions failing across three targets and no TSan diagnostic. It predates subsequent Markdown changes and is historical evidence, not a source-matched pass for this checkpoint.
- **libFuzzer allocator replay:** the final bounded Markdown replay completed 2,248/2,248 executions: the frozen allocator witness, a minimized witness, 38 archived regressions and 2,208 historical inputs. It used Clang 23 objects with a pinned Clang 18 Fuzzer archive and active ASan/LSan/UBSan checks. No sanitizer or Markdown-oracle marker appeared. This establishes bounded non-reproduction after explicit Fuzzer archive linking, not the original runtime root cause. An unavailable 25-byte temporary witness remains untested.
- **Retention and campaigns:** earlier r47/r48 runs passed the 13-target retention protocol within each run; same-binary FTS coverage varied by two PCs between runs. Those results do not establish deterministic coverage or completion of the broader fuzz campaign.
- **Stress and compositor:** the recorded 120-minute chaos run passed functional invariants and failed its PSS-slope budget. The nested Hyprland matrix passed 13 checks twice on one host. These are bounded historical results, not qualification of every supported environment.

Raw logs and manifests remain local and are excluded from product commits. The commands below are recipes for developers to repeat checks. A command in this document does not show that the check passed.

The commands below were not necessarily run during this closeout. Use scratch output and build directories where noted, and do not point tests at a real notes directory.

## Ordinary build and tests

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt unit tests use the offscreen platform through CMake test properties. This is not a live compositor test.

## Static analysis

```sh
HN_STATIC_OUT="$PWD/build-static-out" tests/static/run.sh
```

Runs parser self-test, GCC and Clang clean builds, clang-tidy, and cppcheck. Clang tools run in Docker tools container. Requires build tools and Docker; container image setup is a separate networked operation (`tests/tools/tools-container.sh image`). `HN_STATIC_STAGES="gcc clang tidy cppcheck"` selects stages. `HN_STATIC_SKIP_BUILD=1` explicitly reuses old logs and is not a gate run.

## Sanitizers

Configure sanitizer trees before invoking runner:

```sh
cmake -S . -B build-asan -G Ninja -DHN_SANITIZE=address,undefined
cmake -S . -B build-tsan -G Ninja -DHN_SANITIZE=thread
HN_SANITIZE_OUT="$PWD/build-sanitize-out" tests/sanitize/run.sh asan
HN_SANITIZE_OUT="$PWD/build-sanitize-out" tests/sanitize/run.sh tsan
HN_SANITIZE_OUT="$PWD/build-sanitize-out" tests/sanitize/run.sh valgrind
HN_SANITIZE_OUT="$PWD/build-sanitize-out" tests/sanitize/run.sh heap
```

ThreadSanitizer needs an instrumented Qt to see Qt-internal synchronisation, so the recorded TSan runs build the product with `-DHN_SANITIZE=thread` against a Qt 6.12 built with `-fsanitize=thread` (QtCore, Network, Gui, Widgets, Test, Sql, Concurrent, DBus; its QtGui must be built against the same md4c as the system, 0.6.0, or the Markdown editor tests fail for toolchain reasons). `tests/sanitize/tsan.supp` has two Qt-internal entries for that setup (`QObjectPrivate::maybeSignalConnected`, `QHttpThreadDelegate::~QHttpThreadDelegate`); `tests/sanitize/tsan-controls.sh QT_PREFIX` proves, with Qt-only reproducers in `tests/sanitize/tsan_controls/`, that each entry hides a Qt defect and nothing of ours: the Qt-only programs are reported without the entries and silent with them, and an int, a `std::string` and a lambda-capture-destructor race are still reported with the file loaded (with and without `ignore_interceptors_accesses`). The unsuppressed product reports are kept with the evidence.

Runner rebuilds sanitizer trees when needed. Valgrind and heap modes use the tools container and configure a container build. `all` runs all four stages. Test counts are guarded so empty or incomplete runs fail.

## Coverage

Coverage uses GCC gcov instrumentation and Debug-like settings:

```sh
cmake -S . -B build-cov -G Ninja -DHN_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov
tests/tools/coverage.sh
```

Coverage runner uses `build-cov`, rebuilds, clears old `.gcda` and report files, runs CTest in scratch HOME/XDG/HN directories, then collects line data and HTML/JSON reports. A failed test run still produces diagnostic coverage, but command exits nonzero. Collector needs `gcov` and `gcovr`; install `gcovr` in a disposable Python virtual environment and put that environment's `bin` directory on `PATH`, for example `python3 -m venv "$TMPDIR/hyprnotes-gcovr" && "$TMPDIR/hyprnotes-gcovr/bin/pip" install gcovr && PATH="$TMPDIR/hyprnotes-gcovr/bin:$PATH" tests/tools/coverage.sh`. Do not rely on pre-existing reports or machine-specific log paths.

## Fuzzing

Clang/libFuzzer only:

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DHN_FUZZ=ON
cmake --build build-fuzz
BUILD="$PWD/build-fuzz" tests/fuzz/run.sh 12
```

Argument is minutes per target. Runner executes each target, keeps artifacts under build tree, then asks libFuzzer to merge discovered corpus inputs with `-merge=1 -use_counters=0`. This retains inputs for observed edge coverage without hit-count features; it does not guarantee a globally smallest set or enforce the checked-in corpus file cap. `tests/fuzz/run.sh` checks the file and byte caps after merging. On the 10-minute campaign of 2026-10-17 the reduced markdown, frontmatter, links and manifest corpora had 122, 102, 150 and 64 files and edge coverage equal to or above the full merge. The `markdown_roundtrip` target asserts export/import/export fixed-point stability for every input. Independently, when `NoteEditor` selects Source mode, it asserts byte-for-byte preservation of the original input. These are separate properties, not a proof of general semantic equivalence. The recorded Task 5 campaign used 12 minutes per target; a shorter run is only a smoke campaign and is not equivalent. This is a mutating, time-consuming campaign; do not mistake compile or regression tests for fuzz duration results.

## Stress and chaos

```sh
cmake -S . -B build-stress -G Ninja -DHN_STRESS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-stress
BUILD_DIR="$PWD/build-stress" OUT="$PWD/build-stress-results" CHAOS_MIN=120 tests/stress/run_all.sh
```

Full default stages include `repo_disk`, `app_soak` (20 minutes), and `chaos` (120 minutes). The runner redirects HOME, XDG, and HN storage into temporary directories and removes session scratch on exit. `QUICK=1` shortens runs and is explicitly not valid for reporting. `SOAK_MIN` and `CHAOS_MIN` configure duration. `repo_disk` stage deletes only `hn-*` under `DISK_TMP`; point it at a dedicated scratch directory, not valuable data. Full run can take hours; avoid overlapping it with measurements.

Task 7 recorded failures: original memory/search/idle budgets and PSS flatness at 50/100/200 notes failed; emoji case remains XFAIL. Large-input source fallback measured 17.055 seconds. No broad optimization claim follows from that measurement. The seeded 120-minute chaos result is unresolved; no chaos run is currently active. Source-mode idempotence remains parked.

## Live Wayland / Hyprland checks

Follow [tests/live/README.md](../tests/live/README.md). It uses nested compositor, isolated app state, and scoped cleanup. Live tests require an existing Wayland/Hyprland session and can be invalidated by host display loss. They are opt-in and are not covered by offscreen CTest.

## Recorded validation and pending gates

The following section preserves the October 17 checkpoint. Its status, open gates, and references to “final source” refer to that date. The October 22 status note above describes the later closeout; the earlier failures remain part of the record.

Hyprland-hardening (Part A) status on 2026-10-17: **Part A was not complete.** The entries below identify the tested source and the remaining work at that checkpoint. They do not establish clean-system, cross-compositor, or universal reliability.

### Passing on the final source
- Release CTest 62/62 on the final tree.
- Coverage (snapshot of the final tree, Debug + gcov): 62/62; every module threshold met (core 97.2%, platform 98.0%, plugins 98.7%, app 92.3%, editor 94.9%, theme 97.2%, mods 97.1%; gcovr total 95.1% lines).
- Compiler warnings and static analysis (final snapshot): Clang full build 0 warnings; clang-tidy (main and narrowing passes) and cppcheck clean against the unchanged `tests/static/accepted.txt`.
- Arch package (final snapshot): `check_package.py` 41 checks, 0 failures; `test_bootstrap.sh` 15/15; packaged CLI checks pass without installing anything.
- ThreadSanitizer with an instrumented Qt (final snapshot): **no race report at all** (see below for the 8 tests that still fail on timing/rendering). `tests/sanitize/tsan-controls.sh` passes (Qt-only reproducers reported without the two Qt-internal suppressions and silent with them; int, `std::string` and lambda-capture-destructor races still reported with the file loaded; with and without `ignore_interceptors_accesses`).
- Fuzz (Clang 18, ASan+UBSan, 601 s per target, 13 targets, rerun on the final source 2026-10-17 after all edits; logs in finish-20261017/fuzz/final-source): all 13 targets ran their full time without a crash, hang, timeout or sanitizer report. The Markdown oracle still covers only admitted Visual-mode documents (see limitations).

### Not re-run on the very last edits
- GCC ASan/UBSan/LSan on the final tree (2026-10-17, after all edits): 62/62 pass, no sanitizer diagnostic (log: finish-20261017/final-gates/asan-final.log).
- The Clang/libFuzzer campaign and the end-to-end corpus merge were run before three small final edits (trust-store save guard, NUL query fallback, removal of the older strike boundary entity logic in the Markdown exporter). The `markdown_roundtrip` target in particular has not been fuzzed on the final exporter.

### Earlier results that still stand
- Plugins (Task 8): the 12 behavior requirements are covered by the plugin test set, which passes in Release and under ASan/UBSan.
- Accessibility and layout (Task 9): names, keyboard paths, contrast over seven palettes in light and dark, and picker geometry (1,008 offscreen cases) pass; the Qt file-picker minimum size is still not met at some sizes in a 520x460 viewport.
- Open input limits: the emoji typing case remains an expected failure; a 1M-character source-mode line blocked the event loop for 17 seconds in the earlier measurement.
- Live Hyprland matrix (nested on workspace 4, 2026-10-15): 13 checks passed in two separate invocations with the host workspace unchanged and nothing left running (tests/live/README.md). Not re-run since; later source edits did not touch the harness.
- Stress: 20-minute application soak passed (106 checks, max PSS 121 MiB), 2026-10-16.

### Fixed during the final review (each with a regression test)
- Markdown export no longer turns literal text into link syntax (also across formatting runs); an NBSP-only paragraph is kept; a literal U+FEFF after a backslash escape survives import; a literal `!` before a link, a trailing `#` in a heading and backslashes/`&` in a fence info string are escaped; emphasis and strike delimiters are written so that md4c re-reads them (a letter next to a symbol or punctuation delimiter run becomes a numeric entity). A random-grammar sweep of the exporter went from 481 export/import/export mismatches per 200k inputs to 1 per 900k.
- `RefreshPlugins` reports a busy plugin or an unreadable trust file instead of claiming success. The plugin trust file is never overwritten when it could not be read, a corrupt or wrong-shaped one is copied to `plugins.json.corrupt` first, and a failed rescan keeps the consent already in memory without granting anything new.
- A disabled plugin keeps its settings across rescans; the organizer is placed once and a refusing compositor cannot make the app retry forever; a rename is reported as cancelled only when the user cancelled; the SQLite 3.35 minimum is declared and an unsupported SQLite shows "search unavailable"; the plugin consent dialog shows the whole security warning whenever the screen allows; Clang sanitizer builds get the same timing scale as GCC ones.
- Search: the raw-literal preference (a note containing the typed text verbatim ranks first, then FTS rank) is now answered through a case-sensitive trigram index (schema v3, rebuilt automatically) instead of scanning every candidate body. Result order is byte-identical on the 20 stress queries at 1k and 10k notes and is covered by a brute-force unit test across edits. Cost: cold index time at 10k notes 6.9 s -> 10.7 s, database +6%.

### Historical snapshot from 2026-10-17 (superseded status; original criteria)

The observations in this subsection describe the 2026-10-17 checkpoint. They are retained as history and must not be read as the current Part A status. The 2026-10-21 amendment removed original numeric performance and memory budgets from revised acceptance; the current TSan result and remaining gates are in the ledger linked above. A later TSan full suite completed 84/88 with four timing assertions failing and no TSan diagnostic markers; see `takeover-20261018/tsan-capdrop-full-current-20261021/report.md`.
- **Search latency:** after the index change 24 ms at 1k notes and 60 ms at 10k (was 38 and 229) against 17.7 ms and 46 ms. The remaining time is FTS5 bm25 ranking of ~10k candidates plus the exact-phrase query; closing it needs a ranking or query-plan change that was not approved. Not claimed as passing.
- **W1/W10 memory and idle CPU:** W1 31.2 MiB (28.5), W10 73.4 MiB (64.6), idle CPU 0.017% (exact zero). HEAD under the same toolchain measures 31.8/73.7 MiB and 0.067%: this shows Part A did not worsen them, not that the original budgets are met. The budgets were set on an earlier Qt/toolchain that cannot be reproduced.
- **Massif growth gate:** 0.317 KiB/cycle against < 0.3. The same binary gives 0.317, 0.578 and 0.958 on different runs, so the statistic's snapshot selection is unstable; the deterministic 10,000-cycle heap check reads about 0.10 KiB/cycle (limit 2). The measurement design and the attribution of the growth components remain unresolved; a cycle-indexed Massif sweep was not run. The gate stays failed.
- **ThreadSanitizer:** 54 of 62 pass (was 50, 48 before), with zero race reports. The 8 failures are not races: `core_index` (p95 over the x8 sanitizer scale), `editor_links_test` and `editor_perf_test` (typing p95 14-16 ms against 4 ms, unscaled), `editor_realnotes_test` (load time over target), `app_a11y_test` (120 s timeout), `app_theme_polish_test` (offscreen arrow ink), `app_v2_test` (completion timing), `app_ui_test` (pagination row count 388 vs 300 under TSan timing; passes in Release and ASan). No blanket timing scaling was applied, per decision.
- **Valgrind:** all 36 logs show zero memory errors, but two timing assertions fail under Valgrind (`mods_test` repeated activation 7.4 ms against 4 ms, measured 2026-10-14, and the official tasks plugin Lua budget). They pass in Release and are not waived.
- **Fuzz / Markdown exporter (historical diagnosis):** this checkpoint recorded overlapping emphasis and whitespace-format deltas during the earlier random-grammar sweep. Current `tests/fuzz/fuzz_markdown_roundtrip.cpp` asserts export/import/export fixed point for every input and separately checks byte preservation when `NoteEditor` selects Source mode. Current-source archived inputs and the strict corpus-retention result are recorded above; neither is a full mutation campaign or a universal semantic-equivalence proof. The exact historical deltas and their later dispositions remain in the Part A ledger. A NUL byte in a note body can give a wrong ranking for exactly-3-character queries only if it is still reproduced by current focused tests; consult the ledger before treating this historical note as current.
- **Fuzz corpora:** `run.sh` merges with `-use_counters=0`, retaining inputs for observed edge coverage without hit-count features; merge does not guarantee global minimality or enforce the corpus file cap. The continuation retention runner checks corpus caps explicitly. On the campaign of 2026-10-17 this brought markdown, frontmatter, links and manifest from 383/562/984/215 files to 122/102/150/64 with edge coverage equal or higher, but the full `run.sh` flow was not re-run to completion on the final source.
- **Chaos soak (120 minutes):** the 2026-10-16 run passed 558,592 content checks but failed 108 PSS-slope samples and the exact-zero idle-CPU check; PSS grew from 56.8 to 74.8 MiB while the live malloc heap stayed at 31-35 MiB. A 30-minute rerun with `/proc/PID/smaps` sampling (finish-20261017/chaos-smaps) showed the anonymous footprint bounded at 34-39 MiB with no trend while file-backed PSS swung by 4-8 MiB between samples, and 20 slope failures from that noise; it did not reproduce the 120-minute growth. The 120-minute rerun with smaps sampling (2026-10-17, evidence in finish-20261017/chaos-smaps/120min) still FAILED the gate: 586,755 checks passed, 102 PSS-slope samples failed (minute 15 onward), and the exact-zero idle-CPU check passed this time. Per-mapping accounting shows anonymous PSS rising from 2.3 MiB to 38 MiB in the first 10 minutes, peaking near 45 MiB around minute 80, and ending at 38.8 MiB at minute 120, while file-backed PSS stayed at 21-25 MiB. That is warm-up plus a plateau with no monotonic leak, and it did not reproduce the 2026-10-16 growth to 74.8 MiB, but the slope gate remains failed and is not waived; its design (windowed slope over a fluctuating plateau) and the cause of the earlier growth are unresolved.

### Known limitations (deliberately not changed)
- Ordered lists with more than 256 items were historically split into chunks; a 2026-10-21 regression fix removed that split, and Release plus TSan `editor_roundtrip_test` each passed 79/79. The former limitation is superseded; see the current Part A ledger.
- Hyprland event payloads that are not valid UTF-8 are dropped.
- The consent dialog may need to scroll its warning when the font is very large and the screen small (the warning stays keyboard-reachable).
- Source-mode notes are preserved byte for byte; the serializer is not claimed idempotent for them.
- Live-test harness: a crash between replacing the nested config and saving its digest leaves a session that must be cleaned by hand; the one-shot audit file is not part of the receipt; the workspace-4 holder window is a manual precondition.
