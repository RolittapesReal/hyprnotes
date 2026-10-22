#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile


SCRIPT = pathlib.Path(__file__).with_name("curate_corpus.py")


def invoke(control, semantic, output, report, max_files):
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--control", str(control), "--semantic-dir", str(semantic),
         "--output", str(output), "--report", str(report), "--max-files", str(max_files)],
        text=True, capture_output=True, check=False,
    )


def main():
    with tempfile.TemporaryDirectory(prefix="hn-fuzz-curation-") as tmp:
        root = pathlib.Path(tmp)
        semantic = root / "semantic"
        semantic.mkdir()
        (semantic / "required.json").write_bytes(b"s")
        inputs = [semantic / "required.json"]
        for name, data in [("small", b"x"), ("mid", b"mmm"), ("alternate", b"z")]:
            path = root / name
            path.write_bytes(data)
            inputs.append(path)
        control = root / "merge.control"
        control.write_text(
            f"4\n1\n{inputs[0]}\n{inputs[1]}\n{inputs[2]}\n{inputs[3]}\n"
            "STARTED 0 1\nCOV 0 1\nFT 0 10\n"
            "STARTED 1 1\nCOV 1 2 3\nFT 1 11 12\n"
            "STARTED 2 1\nCOV 2 3 4\nFT 2 12 13\n"
            "STARTED 3 1\nCOV 3 4\nFT 3 13\n"
        )

        output, report = root / "selected", root / "selection.tsv"
        result = invoke(control, semantic, output, report, 3)
        assert result.returncode == 0, result.stderr + result.stdout
        assert sorted(p.name for p in output.iterdir()) == ["alternate", "required.json", "small"]
        assert "coverage_map_entries_retained=4" in report.with_suffix(".coverage.txt").read_text()
        rows = report.read_text().splitlines()
        assert any(row.startswith("selected\t") and "\t2\t" in row for row in rows)
        print("ok   deterministic-coverage-greedy-with-mandatory-seed")

        output, report = root / "over-budget", root / "over-budget.tsv"
        result = invoke(control, semantic, output, report, 1)
        assert result.returncode == 1, result.stdout + result.stderr
        summary = report.with_suffix(".coverage.txt").read_text()
        assert "coverage_map_entries_missing=3" in summary
        assert "coverage-map=1/4 missing=3" in result.stdout
        print("ok   unique-coverage-loss-reported-not-accepted")

        empty_semantic = root / "empty-semantic"
        empty_semantic.mkdir()
        (empty_semantic / "empty.bin").write_bytes(b"")
        output, report = root / "empty-retained", root / "empty.tsv"
        result = invoke(control, empty_semantic, output, report, 1)
        assert result.returncode == 1, result.stdout + result.stderr
        assert (output / "empty.bin").exists()
        print("ok   original-empty-semantic-seed-preserved-outside-libfuzzer-map")


if __name__ == "__main__":
    main()
