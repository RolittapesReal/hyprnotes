#!/usr/bin/env python3
"""Real (unstubbed) process_guard refusal through `lifecycle.py finish`."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
import unittest

import lifecycle_selftest as base

SCRIPT = ("import os,sys,time,pathlib\n"
          "flag=pathlib.Path(sys.argv[1])\n"
          "while not flag.exists(): time.sleep(.01)\n"
          "os.execv(sys.executable,[sys.executable,'-c','import time; time.sleep(90)','mutated'])\n")


class RealGuardRefusal(base.Case):
    def test_identity_changed_after_record_is_refused_by_real_guard(self):
        self.cleanup_child()
        flag = self.base / "mutate-now"
        argv = [sys.executable, "-c", SCRIPT, str(flag)]
        token = pathlib.Path(self.session, "owner-token").read_text()
        self.proc = subprocess.Popen(argv, env={**os.environ, "HN_LIVE_OWNER_TOKEN": token})
        self.configured()
        config = pathlib.Path(self.session, "hyprland.lua").read_bytes()
        request = json.dumps({"argv": argv, "config_sha256": hashlib.sha256(config).hexdigest()})
        self.cli("intent", self.session, "app", str(pathlib.Path(sys.executable).resolve()), data=request)
        self.record()
        flag.write_text("go")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:  # wait until exec changed argv
            if b"mutated" in pathlib.Path(f"/proc/{self.proc.pid}/cmdline").read_bytes():
                break
            time.sleep(.01)
        else:
            self.fail("fixture did not mutate its identity")
        result = self.cli("finish", self.session, check=False)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("guarded stop refused", result.stderr)
        self.assertIsNone(self.proc.poll(), "refused finish must not signal a mismatched process")
        self.assertFalse(pathlib.Path(self.session, "receipt.json").exists())
        self.assertTrue(self.scratch.exists())
        state = json.loads(pathlib.Path(self.session, "state.json").read_text())
        self.assertEqual(state["phase"], "ACTIVE")
        self.assertEqual([r["kind"] for r in state["records"]], ["app"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
