#!/usr/bin/env python3
"""Focused offline proof for matrix's two independently gated float cases."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class FloatMatrixGateTest(unittest.TestCase):
    def test_float_cases_get_distinct_clean_sessions_and_functional_one_is_kept(self):
        with tempfile.TemporaryDirectory(prefix="hn-float-gate-") as tmp:
            root = Path(tmp)
            root.chmod(0o700)
            live = root / "fixture/tests/live"
            checks = live / "checks"
            checks.mkdir(parents=True)
            for name in ("matrix.sh", "preflight.sh", "lifecycle.py", "process_guard.py"):
                shutil.copy(HERE / name, live / name)
            log = root / "float-cases.log"
            script = checks / "float_fallback.sh"
            script.write_text(f'''#!/bin/bash
printf '%s|%s|%s|%s\\n' "$#" "${{1:-}}" "${{2:-}}" "$HN_LIVE_SESSION_DIR" >> {str(log)!r}
case "${{2:-}}" in false) exit 1 ;; true) exit 0 ;; *) exit 91 ;; esac
''')
            script.chmod(0o700)
            bindir = root / "bin"
            bindir.mkdir(mode=0o700)
            hyprctl = bindir / "hyprctl"
            hyprctl.write_text('''#!/bin/sh
case "$*" in
  '-j activeworkspace') echo '{"id":1}' ;;
  '-j workspaces') echo '[{"id":4,"name":"4"}]' ;;
  *) echo '[]' ;;
esac
''')
            hyprctl.chmod(0o700)
            evidence = root / "evidence"
            evidence.mkdir(mode=0o700)
            scratch = root / "scratch"
            scratch.mkdir(mode=0o700)
            env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", TMPDIR=str(scratch),
                       HN_LIVE_EVIDENCE_DIR=str(evidence), HN_LIVE_RESERVE_BYTES="1",
                       HN_LIVE_RESERVE_FILES="1", HYPRLAND_INSTANCE_SIGNATURE="offline-host")
            result = subprocess.run(["bash", str(live / "matrix.sh"), "float_fallback"], env=env,
                                    text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            rows = [line.split("|") for line in log.read_text().splitlines()]
            self.assertEqual([(row[0], row[1], row[2]) for row in rows],
                             [("2", "--case", "false"), ("2", "--case", "true")])
            self.assertNotEqual(rows[0][3], rows[1][3], "float cases reused one durable session")
            sessions = [Path(row[3]) for row in rows]
            self.assertTrue(all(p.is_dir() for p in sessions))
            for session in sessions:
                verified = subprocess.run(["python3", str(live / "lifecycle.py"), "verify-clean", str(session)],
                                          text=True, capture_output=True)
                self.assertEqual(verified.returncode, 0, verified.stderr)
            self.assertIn("FAIL float_fallback", result.stdout)

    def test_standalone_common_exit_before_begin_finalizes_no_launch_session(self):
        with tempfile.TemporaryDirectory(prefix="hn-common-exit-") as tmp:
            root = Path(tmp)
            root.chmod(0o700)
            live = root / "fixture/tests/live"
            checks = live / "checks"
            checks.mkdir(parents=True)
            for name in ("lifecycle.py", "process_guard.py", "nested_hypr.sh", "command_builders.py",
                         "placement_gate.py"):
                shutil.copy(HERE / name, live / name)
            common = checks / "common.sh"
            shutil.copy(HERE / "checks/common.sh", common)
            scratch = root / "scratch"
            scratch.mkdir(mode=0o700)
            evidence = root / "evidence"
            evidence.mkdir(mode=0o700)
            env = dict(os.environ, HN_TASK6_DIR=str(scratch), HN_LIVE_EVIDENCE_DIR=str(evidence),
                       HN_LIVE_INVOCATION_OWNER_PID=str(os.getpid()))
            result = subprocess.run(["bash", "-c", 'source "$1"; exit 1', "fixture", str(common)],
                                    env=env, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            sessions = list(evidence.glob("session-*"))
            self.assertEqual(len(sessions), 1, "standalone EXIT before begin did not create durable session")
            state = json.loads((sessions[0] / "state.json").read_text())
            self.assertEqual(state["phase"], "CLEAN")
            self.assertEqual(state["records"], [])
            verified = subprocess.run(["python3", str(live / "lifecycle.py"), "verify-clean", str(sessions[0])],
                                      capture_output=True, text=True)
            self.assertEqual(verified.returncode, 0, verified.stderr)
            self.assertFalse(scratch.exists(), "controller failed to remove clean no-launch scratch")


if __name__ == "__main__":
    unittest.main(verbosity=2)
