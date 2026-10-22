#!/usr/bin/env python3
"""oneshot-exited: resolves a pending app intent only for a reaped, non-background client."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import unittest

import lifecycle_selftest as base


class OneshotTests(base.Case):
    def intent_argv(self, argv):
        self.configured()
        config = pathlib.Path(self.session, "hyprland.lua").read_bytes()
        request = json.dumps({"argv": argv, "config_sha256": hashlib.sha256(config).hexdigest()})
        self.cli("intent", self.session, "app", str(pathlib.Path(argv[0]).resolve()), data=request)

    def state(self):
        return json.loads(pathlib.Path(self.session, "state.json").read_text())

    def test_reaped_short_lived_client_resolves_pending_intent(self):
        argv = [sys.executable, "--new-note"]
        self.intent_argv(argv)
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait()
        result = self.cli("oneshot-exited", self.session, str(child.pid), check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        st = self.state()
        self.assertIsNone(st["pending"])
        self.assertEqual(st["phase"], "ACTIVE")
        self.assertEqual(st["records"], [])
        audit = list(pathlib.Path(self.session).glob("oneshot-*.json"))
        self.assertEqual(len(audit), 1)
        self.assertEqual(json.loads(audit[0].read_text())["pid"], child.pid)

    def test_live_process_refused_and_intent_retained(self):
        argv = [sys.executable, "--show-organizer"]
        self.intent_argv(argv)
        result = self.cli("oneshot-exited", self.session, str(self.proc.pid), check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIsNotNone(self.state()["pending"])
        self.assertIsNone(self.proc.poll())

    def test_non_allowlisted_argv_cannot_be_abandoned(self):
        argv = [sys.executable, "-c", "pass", "--background"]
        self.intent_argv(argv)
        child = subprocess.Popen(argv)
        child.wait()
        result = self.cli("oneshot-exited", self.session, str(child.pid), check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIsNotNone(self.state()["pending"])

    def test_without_pending_intent_refused(self):
        result = self.cli("oneshot-exited", self.session, "2147483646", check=False)
        self.assertEqual(result.returncode, 2)

    def test_other_single_flag_cannot_be_abandoned(self):
        self.intent_argv([sys.executable, "--settings"])
        child = subprocess.Popen([sys.executable, "-c", "pass"]); child.wait()
        self.assertEqual(self.cli("oneshot-exited", self.session, str(child.pid), check=False).returncode, 2)

    def test_unresolved_intent_still_blocks_finish_without_oneshot(self):
        self.intent_argv([sys.executable, "-c", "pass"])
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)


class ReconfigTests(base.Case):
    def start_compositor(self):
        self.cleanup_child()
        self.configured()
        cfg = str(pathlib.Path(self.session, "hyprland.lua"))
        argv = [sys.executable, "-c", "import time; time.sleep(90)", "-c", cfg]
        token = pathlib.Path(self.session, "owner-token").read_text()
        self.proc = subprocess.Popen(argv, env={**os.environ, "HN_LIVE_OWNER_TOKEN": token})
        digest = hashlib.sha256(pathlib.Path(cfg).read_bytes()).hexdigest()
        request = json.dumps({"argv": argv, "config_sha256": digest})
        self.cli("intent", self.session, "compositor", str(pathlib.Path(sys.executable).resolve()), data=request)
        self.cli("record", self.session, "compositor", str(self.proc.pid))

    def test_reconfig_refused_before_compositor_is_recorded(self):
        self.configured()
        result = self.cli("reconfig", self.session, data="-- new\n", check=False)
        self.assertEqual(result.returncode, 2)

    def test_reconfig_updates_file_and_digest_together_and_session_stays_valid(self):
        self.start_compositor()
        self.assertEqual(self.cli("reconfig", self.session, data="-- scaled\n").returncode, 0)
        cfg = pathlib.Path(self.session, "hyprland.lua")
        self.assertEqual(cfg.read_text(), "-- scaled\n")
        st = json.loads(pathlib.Path(self.session, "state.json").read_text())
        self.assertEqual(st["config_sha256"], hashlib.sha256(cfg.read_bytes()).hexdigest())
        self.assertEqual(self.cli("status", self.session).stdout.strip(), "ACTIVE")
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_second_compositor_refused_while_first_is_live(self):
        self.start_compositor()
        cfg = str(pathlib.Path(self.session, "hyprland.lua"))
        argv = [sys.executable, "-c", "pass", "-c", cfg]
        request = json.dumps({"argv": argv,
                              "config_sha256": hashlib.sha256(pathlib.Path(cfg).read_bytes()).hexdigest()})
        result = self.cli("intent", self.session, "compositor", str(pathlib.Path(sys.executable).resolve()),
                          data=request, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIsNone(json.loads(pathlib.Path(self.session, "state.json").read_text())["pending"])

    def test_reconfig_refused_with_pending_intent_or_empty_config(self):
        self.start_compositor()
        self.assertEqual(self.cli("reconfig", self.session, data="", check=False).returncode, 2)
        config = pathlib.Path(self.session, "hyprland.lua").read_bytes()
        request = json.dumps({"argv": [sys.executable, "-c", "pass"],
                              "config_sha256": hashlib.sha256(config).hexdigest()})
        self.cli("intent", self.session, "app", str(pathlib.Path(sys.executable).resolve()), data=request)
        self.assertEqual(self.cli("reconfig", self.session, data="-- x\n", check=False).returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
