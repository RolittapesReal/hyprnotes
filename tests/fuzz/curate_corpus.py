#!/usr/bin/env python3
"""Select deterministic, semantic-first corpus candidates from libFuzzer merge maps."""

import argparse
import hashlib
import pathlib
import shutil
import sys
from dataclasses import dataclass, field
from fractions import Fraction


@dataclass
class Seed:
    digest: str
    data: bytes
    paths: list[str] = field(default_factory=list)
    cov: set[int] = field(default_factory=set)
    features: set[int] = field(default_factory=set)
    mandatory: bool = False


def parse_control(path: pathlib.Path) -> list[tuple[str, set[int], set[int]]]:
    lines = path.read_text().splitlines()
    if len(lines) < 2:
        raise ValueError("truncated libFuzzer merge control file")
    total = int(lines[0])
    processed = int(lines[1])
    if total < 1 or processed < 0 or len(lines) < 2 + total:
        raise ValueError("invalid libFuzzer merge control header")
    paths = lines[2 : 2 + total]
    cov: dict[int, set[int]] = {}
    features: dict[int, set[int]] = {}
    for line in lines[2 + total :]:
        fields = line.split()
        if len(fields) < 2 or fields[0] not in {"COV", "FT"}:
            continue
        index = int(fields[1])
        if not 0 <= index < total:
            raise ValueError(f"coverage map index out of range: {index}")
        values = {int(value) for value in fields[2:]}
        (cov if fields[0] == "COV" else features).setdefault(index, set()).update(values)
    if processed == 0 or not cov:
        raise ValueError("merge control file contains no coverage maps")
    return [(name, cov.get(i, set()), features.get(i, set())) for i, name in enumerate(paths)]


def load_seeds(control: pathlib.Path, mandatory_dir: pathlib.Path) -> list[Seed]:
    grouped: dict[str, Seed] = {}
    required: set[str] = set()
    for path in mandatory_dir.iterdir():
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"semantic seed is not a regular file: {path}")
        required.add(hashlib.sha256(path.read_bytes()).hexdigest())

    seen_required: set[str] = set()
    for path_text, cov, features in parse_control(control):
        path = pathlib.Path(path_text)
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"merge input is unavailable or not regular: {path}")
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        seed = grouped.setdefault(digest, Seed(digest, data))
        seed.paths.append(path_text)
        seed.cov.update(cov)
        seed.features.update(features)
        seed.mandatory |= digest in required
        if digest in required:
            seen_required.add(digest)
    missing = sorted(required - seen_required)
    for digest in list(missing):
        seed_path = next((path for path in mandatory_dir.iterdir()
                          if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest), None)
        if seed_path is not None and seed_path.stat().st_size == 0:
            grouped[digest] = Seed(digest, b"", [str(seed_path)], mandatory=True)
            missing.remove(digest)
    if missing:
        raise ValueError("semantic seeds absent from control map: " + ", ".join(missing))
    return sorted(grouped.values(), key=lambda seed: (seed.digest, min(seed.paths)))


def select(seeds: list[Seed], max_files: int, max_bytes: int) -> tuple[list[Seed], set[int], set[int], set[int]]:
    chosen = [seed for seed in seeds if seed.mandatory]
    used = {seed.digest for seed in chosen}
    used_bytes = sum(len(seed.data) for seed in chosen)
    if len(chosen) > max_files or used_bytes > max_bytes:
        raise ValueError("mandatory semantic fixtures exceed corpus cap")
    covered = set().union(*(seed.cov for seed in chosen)) if chosen else set()
    all_cov = set().union(*(seed.cov for seed in seeds))
    all_features = set().union(*(seed.features for seed in seeds))
    # Cover rare map entries first. A frequency-1 edge is more informative
    # than a candidate's large pile of already-common edges, preventing broad
    # inputs from starving singleton owners near the file/byte boundary.
    edge_frequency: dict[int, int] = {}
    for seed in seeds:
        for edge in seed.cov:
            edge_frequency[edge] = edge_frequency.get(edge, 0) + 1
    while covered != all_cov and len(chosen) < max_files:
        eligible = []
        for seed in seeds:
            if seed.digest in used or used_bytes + len(seed.data) > max_bytes:
                continue
            new_cov = seed.cov - covered
            if new_cov:
                eligible.append((seed, new_cov, seed.features - set().union(*(s.features for s in chosen)) if chosen else seed.features))
        if not eligible:
            break
        rare_edges = min(edge_frequency[edge] for edge in covered_missing(covered, all_cov))
        rare_candidates = [item for item in eligible if any(
            edge_frequency[edge] == rare_edges for edge in item[1]
        )]
        pool = rare_candidates or eligible
        seed, new_cov, new_features = min(pool, key=lambda item: (
            -sum(1 for edge in item[1] if edge_frequency[edge] == rare_edges),
            -Fraction(len(item[1]), max(1, len(item[0].data))),
            -len(item[1]), -len(item[2]), len(item[0].data), item[0].digest,
            min(item[0].paths),
        ))
        chosen.append(seed)
        used.add(seed.digest)
        used_bytes += len(seed.data)
        covered.update(seed.cov)

    # Deterministic redundancy removal. Mandatory semantic/security seeds are
    # never candidates for removal; nonmandatory inputs are tried largest-first.
    for seed in sorted((item for item in chosen if not item.mandatory),
                       key=lambda item: (-len(item.data), item.digest)):
        remainder = [item for item in chosen if item is not seed]
        if set().union(*(item.cov for item in remainder)) == covered:
            chosen = remainder
            used.remove(seed.digest)
            used_bytes -= len(seed.data)
    covered = set().union(*(seed.cov for seed in chosen)) if chosen else set()

    # After complete edge-map coverage, spend remaining budget on distinct libFuzzer
    # features. Feature-count equality is diagnostic, never a hard acceptance gate.
    feature_union = set().union(*(seed.features for seed in chosen)) if chosen else set()
    while len(chosen) < max_files:
        eligible = [
            seed for seed in seeds
            if seed.digest not in used and len(seed.data) + used_bytes <= max_bytes
            and seed.features - feature_union
        ]
        if not eligible:
            break
        seed = min(eligible, key=lambda item: (
            -Fraction(len(item.features - feature_union), max(1, len(item.data))),
            -len(item.features - feature_union), len(item.data), item.digest, min(item.paths),
        ))
        chosen.append(seed)
        used.add(seed.digest)
        used_bytes += len(seed.data)
        feature_union.update(seed.features)

    return chosen, all_cov, covered, all_features - feature_union


def covered_missing(covered: set[int], all_cov: set[int]) -> set[int]:
    return all_cov - covered


def write_selection(seeds: list[Seed], chosen: list[Seed], out: pathlib.Path, report_path: pathlib.Path,
                    all_cov: set[int], covered: set[int], missing_features: set[int]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise ValueError(f"selection output must be empty: {out}")
    picked = {seed.digest for seed in chosen}
    cov_so_far: set[int] = set()
    ft_so_far: set[int] = set()
    report = ["status\tsha256\tbytes\tcov_entries\tft_features\tmarginal_cov\tmarginal_ft\tsemantic\tsource_paths"]
    for seed in chosen:
        new_cov = seed.cov - cov_so_far
        new_ft = seed.features - ft_so_far
        cov_so_far.update(seed.cov)
        ft_so_far.update(seed.features)
        name = seed.paths[0].split("/")[-1]
        if name in {".", "..", ""}:
            name = seed.digest
        destination = out / name
        if destination.exists():
            destination = out / seed.digest
        shutil.copyfile(seed.paths[0], destination)
        report.append(f"selected\t{seed.digest}\t{len(seed.data)}\t{len(seed.cov)}\t{len(seed.features)}\t{len(new_cov)}\t{len(new_ft)}\t{int(seed.mandatory)}\t{'|'.join(seed.paths)}")
    for seed in seeds:
        if seed.digest not in picked:
            report.append(f"not-selected\t{seed.digest}\t{len(seed.data)}\t{len(seed.cov)}\t{len(seed.features)}\t{len(seed.cov - covered)}\t{len(seed.features - ft_so_far)}\t{int(seed.mandatory)}\t{'|'.join(seed.paths)}")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(report) + "\n")
    report_path.with_suffix(".coverage.txt").write_text(
        f"coverage_map_entries_total={len(all_cov)}\n"
        f"coverage_map_entries_retained={len(covered)}\n"
        f"coverage_map_entries_missing={len(all_cov - covered)}\n"
        f"libfuzzer_features_missing={len(missing_features)}\n"
        f"selected_files={len(chosen)}\n"
        f"selected_bytes={sum(len(seed.data) for seed in chosen)}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", required=True, type=pathlib.Path)
    parser.add_argument("--semantic-dir", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--report", required=True, type=pathlib.Path)
    parser.add_argument("--max-files", type=int, default=199)
    parser.add_argument("--max-bytes", type=int, default=1024 * 1024 - 1)
    args = parser.parse_args()
    try:
        seeds = load_seeds(args.control, args.semantic_dir)
        chosen, all_cov, covered, missing_features = select(seeds, args.max_files, args.max_bytes)
        write_selection(seeds, chosen, args.output, args.report, all_cov, covered, missing_features)
    except (OSError, ValueError) as error:
        print(f"corpus curation failed: {error}", file=sys.stderr)
        return 2
    count = len(chosen)
    size = sum(len(seed.data) for seed in chosen)
    missing_cov = len(all_cov - covered)
    print(f"selected={count} bytes={size} coverage-map={len(covered)}/{len(all_cov)} missing={missing_cov} ft-missing={len(missing_features)}")
    if count >= 200 or size >= 1024 * 1024 or missing_cov:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
