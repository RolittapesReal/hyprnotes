#!/usr/bin/env python3
import gzip
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LIMITS = dict(core=90, platform=90, plugins=85, app=75, editor=85, theme=85, mods=80)


def summarize(files, excluded):
    modules = {name: [0, 0] for name in LIMITS}
    for name, lines in files.items():
        if name in excluded:
            continue
        module = name.split('/')[1]
        if module in modules:
            modules[module][0] += sum(count > 0 for count in lines.values())
            modules[module][1] += len(lines)
    return modules


def failed(modules):
    return any(total == 0 or covered * 100 < LIMITS[name] * total
               for name, (covered, total) in modules.items())


def main():
    excluded = {}
    for line in (ROOT / 'tests/tools/coverage-excluded.txt').read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        name, reason = line.split('\t', 1)
        if not name.startswith('src/') or not reason.strip() or not (ROOT / name).is_file():
            raise ValueError(f'invalid exclusion: {line}')
        excluded[name] = reason
    objects = list((ROOT / 'build-cov').rglob('*.gcno'))
    if not objects or not list((ROOT / 'build-cov').rglob('*.gcda')):
        raise RuntimeError('no coverage instrumentation or execution data')
    files = {}
    with tempfile.TemporaryDirectory(prefix='hyprnotes-gcov-') as scratch:
        for obj in objects:
            subprocess.run(['gcov', '-j', str(obj)], cwd=scratch, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            outputs = list(Path(scratch).glob('*.gcov.json.gz'))
            if not outputs:
                raise RuntimeError(f'gcov produced no JSON for {obj}')
            for output in outputs:
                with gzip.open(output, 'rt') as stream:
                    data = json.load(stream)
                for source in data['files']:
                    path = Path(source['file'])
                    if not path.is_absolute():
                        path = Path(data['current_working_directory']) / path
                    try:
                        name = path.resolve().relative_to(ROOT).as_posix()
                    except ValueError:
                        continue
                    if not name.startswith('src/') or '_autogen/' in name or '/moc_' in name:
                        continue
                    lines = files.setdefault(name, {})
                    for line in source['lines']:
                        number = line['line_number']
                        lines[number] = lines.get(number, 0) + line['count']
                output.unlink()
    if set(excluded) - set(files):
        raise ValueError('exclusion matches no instrumented source')
    modules = summarize(files, excluded)
    report = dict(format='hyprnotes-gcov-lines-v1', modules=modules, exclusions=excluded, files=files)
    (ROOT / 'build-cov/coverage.json').write_text(json.dumps(report, indent=2) + '\n')
    for name, (covered, total) in modules.items():
        percent = 100 * covered / total if total else 0
        print(f'src/{name}: {covered}/{total} {percent:.2f}% (minimum {LIMITS[name]}%)')
    command = ['gcovr', '-r', '.', 'build-cov', '--object-directory', 'build-cov',
               '--filter', 'src/', '--exclude', '.*moc_.*', '--exclude', '.*_autogen.*',
               '--no-markers', '--no-exclude-noncode-lines', '--print-summary',
               '--html-details', 'build-cov/coverage.html',
               '--json', 'build-cov/coverage-gcovr.json']
    for name in excluded:
        command.extend(['--exclude', '^' + re.escape(name) + '$'])
    subprocess.run(command, cwd=ROOT, check=True)
    return int(failed(modules))


if __name__ == '__main__':
    raise SystemExit(main())
