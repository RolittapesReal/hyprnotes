#!/usr/bin/env python3
"""Fail-closed validator for the immutable Task 7 limits and additive metrics."""
import math
import pathlib
import re
import sys

STRICT = {
    "app.W1.pss": ("app.inproc.W1.pss_plus_swappss.p95",),
    "app.W10.pss": ("app.inproc.W10.pss_plus_swappss.p95",),
    "app.idle.cpu": ("app.inproc.W1.idle_cpu_pct_of_one_core", "app.inproc.W10.idle_cpu_pct_of_one_core"),
    "editor.20k.editing.p95": ("editor.20k.typingonly.p95",),
    "index.1000.search.p95": ("index.1000.searchNow_warm.p95",),
    "index.10000.search.p95": ("index.10000.searchNow_warm.p95",),
}

def fail(message):
    print(f"FAIL budget gate: {message}")
    return 1

def main():
    if len(sys.argv) != 3:
        return fail("usage: check_budgets.py BUDGETS RESULT_DIR")
    budget_path, result_dir = map(pathlib.Path, sys.argv[1:])
    if not budget_path.is_file() or not result_dir.is_dir():
        return fail("budget file or result directory missing")
    budgets = {}
    for line_no, line in enumerate(budget_path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3 or fields[0] in budgets:
            return fail(f"malformed or duplicate budget at line {line_no}")
        try:
            value = float(fields[1])
        except ValueError:
            return fail(f"non-numeric budget at line {line_no}")
        if not math.isfinite(value) or value < 0:
            return fail(f"invalid threshold at line {line_no}")
        budgets[fields[0]] = (value, fields[2])
    for name, (limit, unit) in budgets.items():
        sources = STRICT.get(name)
        if sources is None:
            # Non-baseline budgets must identify their exact metric by using the same name.
            sources = (name,)
        found = []
        for log in sorted(result_dir.glob("*.log")):
            for line in log.read_text(errors="replace").splitlines():
                m = re.match(r"METRIC\s+(\S+)\s+([-+0-9.eE]+)\s*(\S*)\s*$", line)
                if m and m.group(1) in sources:
                    found.append((m.group(1), float(m.group(2)), m.group(3) or unit, log.name))
        if not found:
            # A focused stage may not produce every baseline metric. Do not silently skip it.
            return fail(f"required metric {name} missing from {result_dir}")
        if name == "app.idle.cpu":
            required = set(sources)
            present = {metric for metric, *_ in found}
            if required - present:
                return fail(f"required metrics missing for {name}: {', '.join(sorted(required - present))}")
        else:
            found = [row for row in found if row[0] == sources[0]]
        for metric, measured, actual_unit, log in found:
            if actual_unit != unit:
                return fail(f"{metric} unit {actual_unit!r}, expected {unit!r}")
            if not math.isfinite(measured) or measured < 0:
                return fail(f"{metric} has invalid measurement {measured!r} ({log})")
            if measured > limit:
                return fail(f"{metric}={measured:g} {unit} exceeds {limit:g} ({log})")
            print(f"PASS budget {metric}={measured:g} {unit} <= {limit:g}")
    return 0

def self_test():
    import tempfile

    def run_case(budget, metrics, expected):
        with tempfile.TemporaryDirectory(prefix="hyprnotes-budget-test-") as tmp:
            root = pathlib.Path(tmp)
            (root / "budgets.txt").write_text(budget)
            (root / "result").mkdir()
            (root / "result" / "smoke.log").write_text(metrics)
            old_argv = sys.argv
            try:
                sys.argv = [old_argv[0], str(root / "budgets.txt"), str(root / "result")]
                result = main()
            finally:
                sys.argv = old_argv
            if (result == 0) != expected:
                raise AssertionError(f"budget self-test expected {expected}, got {result}")

    run_case("limit 1 ms\n", "METRIC limit 0.5 ms\n", True)
    run_case("limit 1 ms\n", "", False)
    run_case("limit 1 ms\n", "METRIC limit 1.1 ms\n", False)
    run_case("limit 1 ms\n", "METRIC limit nan ms\n", False)
    run_case("limit 1 ms\nlimit 2 ms\n", "METRIC limit 0.5 ms\n", False)
    run_case("app.idle.cpu 0.0 %\n", "METRIC app.inproc.W1.idle_cpu_pct_of_one_core 0 %\n", False)
    run_case("app.idle.cpu 0.0 %\n", "METRIC app.inproc.W1.idle_cpu_pct_of_one_core 0 %\nMETRIC app.inproc.W10.idle_cpu_pct_of_one_core 0 %\n", True)
    print("PASS budget checker self-tests (valid, missing, exceed, non-finite, duplicate, incomplete aliases)")

if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        raise SystemExit(0)
    raise SystemExit(main())
