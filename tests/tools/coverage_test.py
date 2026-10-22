#!/usr/bin/env python3
from coverage import LIMITS, failed, summarize
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import gzip
import json
from unittest.mock import patch
import coverage

modules = {name: [limit, 100] for name, limit in LIMITS.items()}
assert not failed(modules)
for name, limit in LIMITS.items():
    broken = dict(modules, **{name: [limit - 1, 100]})
    assert failed(broken), name
    assert failed(dict(modules, **{name: [0, 0]})), name
assert failed(summarize({}, {}))
files = {'src/core/a.cpp': {1: 1, 2: 0}, 'src/core/b.cpp': {1: 0}}
assert summarize(files, {})['core'] == [1, 3]
assert summarize(files, {'src/core/b.cpp': 'paint only'})['core'] == [1, 2]
with tempfile.TemporaryDirectory(prefix='hyprnotes-coverage-gate-test-') as scratch:
    root = Path(scratch)
    (root / 'tests/tools').mkdir(parents=True)
    (root / 'build-cov').mkdir()
    (root / 'bin').mkdir()
    shutil.copy(Path(__file__).with_name('coverage.sh'), root / 'tests/tools/coverage.sh')
    (root / 'build-cov/CMakeCache.txt').write_text('HN_COVERAGE:BOOL=ON\n')
    commands = {
        'cmake': 'exit 0',
        'ctest': 'test "$HN_NOTES_DIR" = "$HOME/Notes/Hyprnotes" || exit 91\ntest -d "$XDG_RUNTIME_DIR" || exit 92\nexit "$TEST_CTEST_RC"',
        'python3': 'test ! -e build-cov/coverage.json || exit 90\ntest ! -e build-cov/coverage.html || exit 93\ntest ! -e build-cov/coverage-gcovr.json || exit 94\nprintf fresh > build-cov/coverage.json\nexit "$TEST_COLLECT_RC"',
    }
    for name, body in commands.items():
        command = root / 'bin' / name
        command.write_text('#!/bin/sh\n' + body + '\n')
        command.chmod(0o755)
    for test_rc, collect_rc in [(0, 0), (1, 0), (0, 1), (1, 1)]:
        report = root / 'build-cov/coverage.json'
        report.write_text('stale')
        (root / 'build-cov/coverage.html').write_text('stale HTML')
        (root / 'build-cov/coverage-gcovr.json').write_text('stale gcovr JSON')
        env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                   TEST_CTEST_RC=str(test_rc), TEST_COLLECT_RC=str(collect_rc))
        result = subprocess.run(['bash', str(root / 'tests/tools/coverage.sh')],
                                env=env, capture_output=True, text=True, check=False)
        assert (result.returncode != 0) == bool(test_rc or collect_rc)
        assert report.read_text() == 'fresh'
        assert ('coverage report is diagnostic' in result.stderr) == bool(test_rc)
with tempfile.TemporaryDirectory(prefix='hyprnotes-coverage-collector-test-') as scratch:
    root = Path(scratch)
    (root / 'tests/tools').mkdir(parents=True)
    (root / 'build-cov').mkdir()
    (root / 'src/core').mkdir(parents=True)
    (root / 'src/core/a.cpp').write_text('int a;\n')
    (root / 'src/core/unmeasured.cpp').write_text('int b;\n')
    exclusions = root / 'tests/tools/coverage-excluded.txt'
    exclusions.write_text('')
    (root / 'build-cov/one.gcno').touch()
    (root / 'build-cov/two.gcno').touch()
    (root / 'build-cov/one.gcda').touch()

    def run_tool(command, cwd, check, **kwargs):
        if command[0] == 'gcov':
            first = command[2].endswith('one.gcno')
            source = 'src/core/a.cpp' if first else str(root / 'src/core/../core/a.cpp')
            data = dict(current_working_directory=str(root), files=[dict(
                file=source, lines=[dict(line_number=1, count=0 if first else 2),
                                    dict(line_number=2, count=0)])])
            with gzip.open(Path(cwd) / 'sample.gcov.json.gz', 'wt') as stream:
                json.dump(data, stream)
        else:
            assert '--html-details' in command
            assert '--no-markers' in command
            assert '--no-exclude-noncode-lines' in command
            (root / 'build-cov/coverage.html').write_text('fresh HTML')
            (root / 'build-cov/coverage-gcovr.json').write_text('{}')
        return subprocess.CompletedProcess(command, 0)

    with patch.object(coverage, 'ROOT', root), patch.object(coverage.subprocess, 'run', run_tool):
        assert coverage.main() == 1  # Missing modules remain failures.
        report = json.loads((root / 'build-cov/coverage.json').read_text())
        assert report['files'] == {'src/core/a.cpp': {'1': 2, '2': 0}}
        assert report['modules']['core'] == [1, 2]
        assert (root / 'build-cov/coverage.html').read_text() == 'fresh HTML'
        exclusions.write_text('src/core/a.cpp\tpaint fixture\n')
        assert coverage.main() == 1
        assert json.loads((root / 'build-cov/coverage.json').read_text())['modules']['core'] == [0, 0]
        for invalid in ['src/core/a.cpp\t\n', 'src/core/a.cpp\n',
                        'src/core/unmeasured.cpp\tno instrumented lines\n']:
            exclusions.write_text(invalid)
            try:
                coverage.main()
            except ValueError:
                pass
            else:
                raise AssertionError(f'accepted invalid exclusion: {invalid!r}')
        exclusions.write_text('')

        def failing_html(command, **kwargs):
            if command[0] == 'gcovr':
                raise subprocess.CalledProcessError(2, command)
            return run_tool(command, **kwargs)

        with patch.object(coverage.subprocess, 'run', failing_html):
            try:
                coverage.main()
            except subprocess.CalledProcessError:
                pass
            else:
                raise AssertionError('HTML tool failure did not fail collection')
print('coverage gate checks passed')
