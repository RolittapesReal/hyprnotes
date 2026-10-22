#!/usr/bin/env python3
"""Offline lifecycle-controller tests. All process fixtures are owned by tests."""
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
CTL = HERE / "lifecycle.py"


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hn-life-")
        self.base = pathlib.Path(self.tmp.name)
        os.chmod(self.base, 0o700)
        self.ev = self.base / "evidence"
        self.ev.mkdir(mode=0o700)
        self.scratch = self.base / "scratch"
        self.scratch.mkdir(mode=0o700)
        self.session = self.cli("init", str(self.ev), str(self.scratch), str(os.getpid())).stdout.strip()
        token = pathlib.Path(self.session, "owner-token").read_text()
        self.proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(90)"],
                                     env={**os.environ, "HN_LIVE_OWNER_TOKEN": token})
        self.addCleanup(self.cleanup_child)

    def cleanup_child(self):
        if getattr(self, "proc", None) and self.proc.poll() is None:
            self.proc.terminate()
        if getattr(self, "proc", None):
            try: self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait()

    def tearDown(self):
        self.cleanup_child()
        self.tmp.cleanup()

    def cli(self, *args, data=None, check=True, env=None):
        p = subprocess.run([sys.executable, str(CTL), *args], input=data, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env={**os.environ, **(env or {})})
        if check and p.returncode:
            self.fail(f"CLI {args[0]} exited {p.returncode}: {p.stderr}")
        return p

    def configured(self):
        self.cli("config", self.session, data="# offline config\n")

    def intent(self, kind="app"):
        self.configured()
        config = pathlib.Path(self.session, "hyprland.lua").read_bytes()
        executable = pathlib.Path(sys.executable).resolve()
        arg = json.dumps({"argv": [sys.executable, "-c", "import time; time.sleep(90)"],
                          "config_sha256": hashlib.sha256(config).hexdigest()})
        return self.cli("intent", self.session, kind, str(executable), data=arg).stdout.strip()

    def record(self, pid=None):
        return self.cli("record", self.session, "app", str(pid or self.proc.pid))

    def crash_finish(self, point):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        (self.scratch / "fixture-owned-data").write_text("discardable")
        p = self.drive_controller_boundary(point, "finish", self.session)
        self.assertEqual(p.returncode, 86, (point, p.stderr))
        return json.loads(pathlib.Path(self.session, "state.json").read_text())

    def drive_controller_boundary(self, point, *args, data=None):
        return subprocess.run([sys.executable, str(HERE / "lifecycle_selftest.py"), "--boundary", point, *args],
                              input=data, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class LifecycleTests(Case):
    def test_evidence_root_rejects_symlink_overlap_and_bad_mode(self):
        link = self.base / "link"; link.symlink_to(self.ev)
        self.assertNotEqual(self.cli("init", str(link), str(self.scratch), str(os.getpid()), check=False).returncode, 0)
        self.assertNotEqual(self.cli("init", str(self.base), str(self.scratch), str(os.getpid()), check=False).returncode, 0)
        os.chmod(self.ev, 0o755)
        self.assertNotEqual(self.cli("init", str(self.ev), str(self.scratch), str(os.getpid()), check=False).returncode, 0)

    def test_unsafe_writable_ancestor_rejected_before_setup_side_effects(self):
        unsafe = self.base / "unsafe"; unsafe.mkdir(mode=0o700); os.chmod(unsafe, 0o777)
        ev = unsafe / "evidence"; ev.mkdir(mode=0o700)
        before = {p.name for p in self.base.glob(".hn-stage-*")}
        result = self.cli("init", str(ev), str(self.scratch), str(os.getpid()), check=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual({p.name for p in self.base.glob(".hn-stage-*")}, before)
        self.assertFalse(list(ev.glob("session-*")))

    def test_root_owned_sticky_temporary_ancestor_is_allowed(self):
        result = self.cli("init", str(self.ev), str(self.scratch), str(os.getpid()), check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        state = json.loads(pathlib.Path(result.stdout.strip(), "state.json").read_text())
        self.assertEqual(state["phase"], "PREPARED")

    def test_invalid_owner_creates_no_stage_session_or_init_record(self):
        stages = {p.name for p in self.base.glob(".hn-stage-*")}
        sessions = {p.name for p in self.ev.glob("session-*")}
        intents = {p.name for p in self.ev.glob("init-*.json")}
        result = self.cli("init", str(self.ev), str(self.scratch), "2147483647", check=False)
        self.assertEqual(result.returncode, 2)
        self.assertEqual({p.name for p in self.base.glob(".hn-stage-*")}, stages)
        self.assertEqual({p.name for p in self.ev.glob("session-*")}, sessions)
        self.assertEqual({p.name for p in self.ev.glob("init-*.json")}, intents)

    def test_init_crash_boundaries_leave_durable_bounded_setup_intent(self):
        for point in ("after_init_intent", "after_init_session_mkdir", "after_init_stage_mkdir", "after_init_stage_result"):
            with self.subTest(point=point):
                evidence = self.base / ("evidence-" + point); evidence.mkdir(mode=0o700)
                scratch = self.base / ("scratch-" + point); scratch.mkdir(mode=0o700)
                result = self.drive_controller_boundary(point, "init", str(evidence), str(scratch), str(os.getpid()))
                self.assertEqual(result.returncode, 86, (point, result.stderr))
                intents = list(evidence.glob("init-*.json")); self.assertEqual(len(intents), 1)
                intent = json.loads(intents[0].read_text())
                session = evidence / intent["session"]
                if point == "after_init_intent":
                    self.assertFalse(session.exists())
                    self.assertFalse(pathlib.Path(intent["stage"]).exists())
                elif point == "after_init_session_mkdir":
                    self.assertTrue(session.is_dir())
                    self.assertFalse(pathlib.Path(session, "state.json").exists())
                    self.assertFalse(pathlib.Path(intent["stage"]).exists())
                elif point == "after_init_stage_mkdir":
                    state = json.loads(pathlib.Path(session, "state.json").read_text())
                    self.assertEqual(state["phase"], "UNSAFE")
                    self.assertEqual(state["setup"]["status"], "intent")
                    self.assertTrue(pathlib.Path(intent["stage"]).is_dir())
                else:
                    state = json.loads(pathlib.Path(session, "state.json").read_text())
                    self.assertEqual(state["phase"], "PREPARED")
                    self.assertEqual(state["setup"]["status"], "ready")
                    self.assertEqual(state["stage_id"]["ino"], os.stat(intent["stage"]).st_ino)

    def test_duplicate_json_keys_and_malformed_state_types_fail_before_use(self):
        state_path = pathlib.Path(self.session, "state.json")
        original = state_path.read_bytes()
        try:
            text = original.decode()
            text = text.replace('"phase":"PREPARED"', '"phase":"UNSAFE","phase":"PREPARED"')
            state_path.write_text(text)
            duplicate_bytes = state_path.read_bytes()
            self.assertEqual(self.cli("status", self.session, check=False).returncode, 2)
            self.assertEqual(state_path.read_bytes(), duplicate_bytes)
            state_path.write_bytes(original)
            state = json.loads(original); state["owner_pid"] = [os.getpid()]
            state_path.write_text(json.dumps(state))
            self.assertEqual(self.cli("status", self.session, check=False).returncode, 2)
        finally:
            state_path.write_bytes(original)

    def test_malformed_state_ids_and_nested_bindings_fail_closed_unchanged(self):
        state_path = pathlib.Path(self.session, "state.json")
        original = state_path.read_bytes()
        mutations = [
            lambda s: s.update(invocation="not-hex"),
            lambda s: s.update(owner_start={"tick": 1}),
            lambda s: s.update(evidence_id={"dev": [], "ino": 1}),
            lambda s: s.update(scratch_id={"dev": -1, "ino": 1}),
            lambda s: s.update(parent="/tmp/forged-parent"),
            lambda s: s.update(pending={"id": "bad", "kind": "app", "executable": "/bin/false", "argv": ["/bin/false"]}, phase="PENDING"),
            lambda s: s.update(setup={"status": "ready", "stage_name": "../../outside", "init_record": "bad"}),
        ]
        try:
            for mutate in mutations:
                state = json.loads(original); mutate(state)
                malformed = json.dumps(state).encode()
                state_path.write_bytes(malformed)
                result = self.cli("status", self.session, check=False)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(state_path.read_bytes(), malformed)
                self.assertTrue(self.scratch.exists())
        finally: state_path.write_bytes(original)

    def test_malformed_state_path_substitution_preserves_original_bytes(self):
        state_path = pathlib.Path(self.session, "state.json")
        original = state_path.read_bytes()
        state = json.loads(original); state["scratch"] = ["/tmp"]
        state_path.write_text(json.dumps(state))
        malformed = state_path.read_bytes()
        try:
            self.assertEqual(self.cli("status", self.session, check=False).returncode, 2)
            self.assertEqual(state_path.read_bytes(), malformed)
            self.assertTrue(self.scratch.exists())
        finally: state_path.write_bytes(original)

    def test_pending_intent_never_signals(self):
        self.intent()
        self.assertIsNone(self.proc.poll())
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertIsNone(self.proc.poll())

    def test_production_cli_ignores_environment_failpoint_controls(self):
        self.intent(); self.record()
        attempted = self.cli("finish", self.session, check=False,
                             env={"HN_LIFECYCLE_TESTING": "1", "HN_LIFECYCLE_FAILPOINT": "after_guarded_signal"})
        self.assertEqual(attempted.returncode, 0)
        self.assertTrue(pathlib.Path(self.session, "receipt.json").exists())

    def test_record_commit_preserves_guard_identity(self):
        self.intent(); self.record()
        state = json.loads(pathlib.Path(self.session, "state.json").read_text())
        rows = list(pathlib.Path(self.session).glob("record-*.json"))
        self.assertEqual(len(rows), 1)
        row = json.loads(rows[0].read_text())
        self.assertEqual(row["pid"], self.proc.pid)
        self.assertEqual(state["records"][0]["sha256"], hashlib.sha256(rows[0].read_bytes()).hexdigest())

    def test_finish_refusal_and_live_owner_retain_state(self):
        self.intent()
        before = set(pathlib.Path(self.session).iterdir())
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertTrue(before <= set(pathlib.Path(self.session).iterdir()))
        self.assertTrue(self.scratch.exists())

    def test_confirmed_exit_clean_receipt_is_idempotent(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        self.assertFalse(self.scratch.exists())
        receipt = pathlib.Path(self.session, "receipt.json").read_bytes()
        self.cli("finish", self.session); self.assertEqual(receipt, pathlib.Path(self.session, "receipt.json").read_bytes())
        self.assertEqual(self.cli("verify-clean", self.session).returncode, 0)

    def test_partial_write_and_concurrent_finalize_fail_closed(self):
        self.intent()
        lockfd = os.open(pathlib.Path(self.session, "lock"), os.O_RDWR)
        import fcntl
        fcntl.flock(lockfd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
            self.assertTrue(self.scratch.exists())
        finally:
            fcntl.flock(lockfd, fcntl.LOCK_UN); os.close(lockfd)
        state = pathlib.Path(self.session, "state.json")
        original = state.read_bytes(); state.write_bytes(b"{")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertEqual(state.read_bytes(), b"{")
        state.write_bytes(original)

    def test_guard_inputs_private_and_config_path_stable(self):
        self.intent(); self.record()
        cfg = pathlib.Path(self.session, "hyprland.lua")
        self.assertEqual(cfg.stat().st_mode & 0o777, 0o600)
        self.assertEqual(pathlib.Path(self.session, "owner-token").stat().st_mode & 0o777, 0o600)
        row = json.loads(next(pathlib.Path(self.session).glob("record-*.json")).read_text())
        self.assertEqual(row["config"], str(cfg))
        self.assertNotIn("fixture-token", self.cli("status", self.session).stdout + self.cli("status", self.session).stderr)

    def test_config_digest_mutation_after_intent_cannot_commit_record(self):
        self.intent()
        pathlib.Path(self.session, "hyprland.lua").write_text("-- swapped config\n")
        attempted = self.cli("record", self.session, "app", str(self.proc.pid), check=False)
        self.assertEqual(attempted.returncode, 2)
        self.assertIsNone(self.proc.poll())
        state = json.loads(pathlib.Path(self.session, "state.json").read_text())
        self.assertIsNotNone(state["pending"])

    def test_crash_between_record_and_promotion_never_recovers_from_current_pid(self):
        self.intent()
        # Simulate durable record orphaned before state promotion.
        pathlib.Path(self.session, "record-orphan.json").write_text("{}")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertIsNone(self.proc.poll())

    def test_cross_session_receipt_rejected(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        other_scratch = self.base / "other-scratch"; other_scratch.mkdir(mode=0o700)
        other = self.cli("init", str(self.ev), str(other_scratch), str(os.getpid())).stdout.strip()
        pathlib.Path(other, "receipt.json").write_bytes(pathlib.Path(self.session, "receipt.json").read_bytes())
        self.assertEqual(self.cli("verify-clean", other, check=False).returncode, 2)

    def test_proc_inaccessible_and_reused_pid_are_unknown(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("lifecycle_candidate", CTL)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        row = {"pid": 424242, "uid": os.getuid(), "start": "old"}
        old_uid, old_start = module.proc_uid, module.proc_start
        try:
            module.proc_uid = lambda _pid: (_ for _ in ()).throw(PermissionError())
            self.assertFalse(module.positive_exit(row))
            module.proc_uid = lambda _pid: os.getuid()
            module.proc_start = lambda _pid: "new"
            self.assertFalse(module.positive_exit(row))
        finally:
            module.proc_uid, module.proc_start = old_uid, old_start

    def test_signal_then_receipt_crash_revalidates_exit(self):
        self.intent(); self.record()
        crashed = self.drive_controller_boundary("after_guarded_signal", "finish", self.session)
        self.assertEqual(crashed.returncode, 86)
        self.proc.wait(timeout=2)
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        self.assertEqual(self.cli("verify-clean", self.session).returncode, 0)

    def test_scratch_replaced_between_validation_and_staging_preserves_foreign_tree(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        # Inode substitution before finalization.
        old = self.base / "old"; self.scratch.rename(old); self.scratch.mkdir(mode=0o700)
        (self.scratch / "valuable").write_text("keep")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertEqual((self.scratch / "valuable").read_text(), "keep")

    def test_scratch_symlink_and_mode_change_abort_without_fallback(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        original = self.base / "original"; self.scratch.rename(original); self.scratch.symlink_to(original)
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertTrue((original).is_dir())

    def test_partial_descriptor_cleanup_preserves_recovery_state(self):
        self.intent(); self.record()
        self.proc.terminate(); self.proc.wait(timeout=2)
        os.chmod(self.scratch, 0o755)
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertTrue(pathlib.Path(self.session, "state.json").exists())
        self.assertTrue(self.scratch.exists())

    def test_killed_mid_descriptor_deletion_retains_proof_and_safely_resumes(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        (self.scratch / "first-owned").write_text("remove first")
        (self.scratch / "still-owned").write_text("retain until recovery")
        crashed = self.drive_controller_boundary("after_first_owned_unlink", "finish", self.session)
        self.assertEqual(crashed.returncode, 86, crashed.stderr)
        state_path = pathlib.Path(self.session, "state.json")
        state = json.loads(state_path.read_text())
        tree_name = state.get("staging", {}).get("name", "tree-" + pathlib.Path(self.session).name)
        staged_tree = pathlib.Path(state["stage"], tree_name)
        self.assertEqual(state["phase"], "FINALIZING")
        self.assertFalse((staged_tree / "first-owned").exists())
        self.assertEqual((staged_tree / "still-owned").read_text(), "retain until recovery")
        self.assertTrue(state["records"])
        self.assertFalse(pathlib.Path(self.session, "receipt.json").exists())
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        self.assertTrue(pathlib.Path(self.session, "receipt.json").exists())

    def test_deletion_receipt_crash_does_not_invent_clean(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        receipt = pathlib.Path(self.session, "receipt.json")
        receipt.write_text("{")
        self.assertEqual(self.cli("verify-clean", self.session, check=False).returncode, 2)

    def test_killed_after_durable_staging_intent_reenters_from_bound_source(self):
        st = self.crash_finish("after_staging_intent")
        self.assertEqual(st["staging"]["status"], "intent")
        self.assertTrue((self.scratch / "fixture-owned-data").exists())
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_killed_after_rename_before_result_validates_staged_inode_then_resumes(self):
        st = self.crash_finish("after_staging_rename")
        stage_tree = pathlib.Path(st["stage"], st["staging"]["name"])
        self.assertTrue(stage_tree.is_dir())
        self.assertFalse(self.scratch.exists())
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_reentry_after_rename_preserves_replaced_source_and_foreign_staging_entries(self):
        st = self.crash_finish("after_staging_rename")
        (self.scratch).mkdir(mode=0o700)
        (self.scratch / "foreign-source").write_text("preserve")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertEqual((self.scratch / "foreign-source").read_text(), "preserve")
        self.assertEqual(pathlib.Path(st["stage"], st["staging"]["name"], "fixture-owned-data").read_text(), "discardable")

    def test_reentry_after_committed_rename_rejects_replaced_source(self):
        st = self.crash_finish("after_staging_result")
        self.scratch.mkdir(mode=0o700)
        (self.scratch / "foreign-source").write_text("preserve")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertEqual((self.scratch / "foreign-source").read_text(), "preserve")
        self.assertEqual(pathlib.Path(st["stage"], st["staging"]["name"], "fixture-owned-data").read_text(), "discardable")

    def test_scratch_substituted_after_validation_before_rename_preserves_both_trees(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        (self.scratch / "owned-tree").mkdir()
        (self.scratch / "owned-tree" / "keep").write_text("original")
        foreign = self.base / "foreign-scratch"
        foreign.mkdir(mode=0o700); (foreign / "keep").write_text("foreign")
        result = self.drive_controller_boundary("race_source_before_rename", "finish", self.session)
        self.assertEqual(result.returncode, 2, result.stderr)
        state = json.loads(pathlib.Path(self.session, "state.json").read_text())
        tree = pathlib.Path(state["stage"], state["staging"]["name"])
        self.assertEqual((tree / "keep").read_text(), "foreign")
        self.assertEqual((self.base / "original-scratch" / "owned-tree" / "keep").read_text(), "original")

    def test_child_swap_between_stat_and_open_preserves_foreign_tree(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        (self.scratch / "victim").mkdir(); (self.scratch / "victim" / "expected").write_text("owned")
        (self.scratch / "foreign").mkdir(); (self.scratch / "foreign" / "keep").write_text("foreign")
        result = self.drive_controller_boundary("race_open_swap", "finish", self.session)
        self.assertEqual(result.returncode, 2, (result.stdout, result.stderr))
        st = json.loads(pathlib.Path(self.session, "state.json").read_text())
        tree = pathlib.Path(st["stage"], st.get("staging", {}).get("name", "tree-" + pathlib.Path(self.session).name))
        self.assertEqual((tree / "victim" / "keep").read_text(), "foreign")
        self.assertEqual((tree / "displaced" / "expected").read_text(), "owned")

    def test_child_swap_after_traversal_before_rmdir_preserves_foreign_tree(self):
        self.intent(); self.record(); self.proc.terminate(); self.proc.wait(timeout=2)
        (self.scratch / "victim").mkdir()
        (self.scratch / "foreign").mkdir(); (self.scratch / "foreign" / "keep").write_text("foreign")
        result = self.drive_controller_boundary("race_before_rmdir", "finish", self.session)
        self.assertEqual(result.returncode, 2)
        st = json.loads(pathlib.Path(self.session, "state.json").read_text())
        tree = pathlib.Path(st["stage"], st.get("staging", {}).get("name", "tree-" + pathlib.Path(self.session).name))
        self.assertEqual((tree / "victim" / "keep").read_text(), "foreign")
        self.assertTrue((tree / "displaced").is_dir())

    def test_unexpected_staging_entry_aborts_without_deleting_either_tree(self):
        st = self.crash_finish("after_staging_intent")
        foreign = pathlib.Path(st["stage"], "unowned-entry")
        foreign.write_text("preserve")
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertTrue(self.scratch.exists())
        self.assertEqual(foreign.read_text(), "preserve")

    def test_malformed_staging_journal_is_preserved_and_never_authorizes_cleanup(self):
        st = self.crash_finish("after_staging_intent")
        state_path = pathlib.Path(self.session, "state.json")
        st["staging"]["unrecognized"] = True
        state_path.write_text(json.dumps(st))
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertTrue(self.scratch.exists())
        self.assertTrue(pathlib.Path(st["stage"], st["staging"]["name"]).exists() is False)

    def test_killed_after_staging_result_reenters_without_pid_promotion(self):
        st = self.crash_finish("after_staging_result")
        self.assertEqual(st["staging"]["status"], "renamed")
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_killed_after_deletion_intent_revalidates_inode_and_resumes(self):
        st = self.crash_finish("after_deletion_intent")
        self.assertEqual(st["staging"]["deletion"], "deleting")
        self.assertTrue(pathlib.Path(st["stage"], st["staging"]["name"], "fixture-owned-data").exists())
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_killed_after_tree_removal_without_durable_proof_stays_unknown(self):
        st = self.crash_finish("after_tree_removal")
        self.assertEqual(st["staging"]["deletion"], "deleting")
        self.assertFalse(pathlib.Path(st["stage"], st["staging"]["name"]).exists())
        self.assertEqual(self.cli("finish", self.session, check=False).returncode, 2)
        self.assertFalse(pathlib.Path(self.session, "receipt.json").exists())

    def test_killed_after_deletion_proof_reenters_receipt_commit(self):
        st = self.crash_finish("after_deletion_result")
        self.assertEqual(st["staging"]["deletion"], "removed")
        self.assertEqual(self.cli("finish", self.session).returncode, 0)

    def test_killed_after_receipt_commit_reconciles_state_only_from_receipt(self):
        st = self.crash_finish("after_receipt_commit")
        self.assertEqual(st["phase"], "FINALIZING")
        self.assertTrue(pathlib.Path(self.session, "receipt.json").exists())
        self.assertEqual(self.cli("finish", self.session).returncode, 0)
        self.assertEqual(self.cli("verify-clean", self.session).returncode, 0)


def run_test_kill_driver(argv):
    if len(argv) < 4 or argv[1] != "--boundary": return False
    import importlib.util
    point = argv[2]
    spec = importlib.util.spec_from_file_location("lifecycle_test_subject", CTL)
    subject = importlib.util.module_from_spec(spec); spec.loader.exec_module(subject)

    def die(): os._exit(86)

    injected = {"done": False}
    if point in ("after_staging_intent", "after_staging_result", "after_deletion_intent", "after_deletion_result", "after_init_stage_result"):
        original = subject.save
        def save(session, state):
            original(session, state)
            tx = state.get("staging") or {}
            if point == "after_staging_intent" and tx.get("status") == "intent": die()
            if point == "after_staging_result" and tx.get("status") == "renamed": die()
            if point == "after_deletion_intent" and tx.get("deletion") == "deleting": die()
            if point == "after_deletion_result" and tx.get("deletion") == "removed": die()
            if point == "after_init_stage_result" and state.get("phase") == "PREPARED" and state.get("setup", {}).get("status") == "ready": die()
        subject.save = save
    elif point == "after_staging_rename":
        original = subject.os.rename
        def rename(*args, **kwargs):
            result = original(*args, **kwargs)
            if kwargs.get("src_dir_fd") is not None and kwargs.get("dst_dir_fd") is not None: die()
            return result
        subject.os.rename = rename
    elif point == "after_tree_removal":
        original = subject.os.rmdir
        def rmdir(name, *args, **kwargs):
            result = original(name, *args, **kwargs)
            if isinstance(name, str) and name.startswith("tree-"): die()
            return result
        subject.os.rmdir = rmdir
    elif point == "after_receipt_commit":
        original = subject.exclusive_json
        def exclusive(path, obj):
            result = original(path, obj)
            if path.endswith("/receipt.json"): die()
            return result
        subject.exclusive_json = exclusive
    elif point == "after_init_intent":
        original = subject.exclusive_json
        def exclusive(path, obj):
            result = original(path, obj)
            if os.path.basename(path).startswith("init-"): die()
            return result
        subject.exclusive_json = exclusive
    elif point == "after_init_stage_mkdir":
        original = subject.os.mkdir
        def mkdir(path, *args, **kwargs):
            result = original(path, *args, **kwargs)
            if os.path.basename(path).startswith(".hn-stage-"): die()
            return result
        subject.os.mkdir = mkdir
    elif point == "after_init_session_mkdir":
        original = subject.os.mkdir
        def mkdir(path, *args, **kwargs):
            result = original(path, *args, **kwargs)
            if os.path.basename(path).startswith("session-"): die()
            return result
        subject.os.mkdir = mkdir
    elif point == "after_guarded_signal":
        original = subject.process_guard.signal_record
        def signal_record(*args, **kwargs):
            result = original(*args, **kwargs)
            if result: die()
            return result
        subject.process_guard.signal_record = signal_record
    elif point == "race_open_swap":
        original_open, original_rename, original_listdir = subject.os.open, subject.os.rename, subject.os.listdir
        def listdir(fd):
            names = original_listdir(fd)
            return ["victim"] + [n for n in names if n != "victim"] if "victim" in names else names
        subject.os.listdir = listdir
        def open_entry(path, flags, *args, **kwargs):
            if path == "victim" and kwargs.get("dir_fd") is not None and not injected["done"]:
                fd = kwargs["dir_fd"]
                original_rename("victim", "displaced", src_dir_fd=fd, dst_dir_fd=fd)
                original_rename("foreign", "victim", src_dir_fd=fd, dst_dir_fd=fd)
                injected["done"] = True
            return original_open(path, flags, *args, **kwargs)
        subject.os.open = open_entry
    elif point == "race_before_rmdir":
        original_stat, original_rename, original_listdir = subject.os.stat, subject.os.rename, subject.os.listdir
        def listdir(fd):
            names = original_listdir(fd)
            return ["victim"] + [n for n in names if n != "victim"] if "victim" in names else names
        subject.os.listdir = listdir
        counts = {}
        def stat_entry(path, *args, **kwargs):
            if path == "victim" and kwargs.get("dir_fd") is not None:
                key = kwargs["dir_fd"]; counts[key] = counts.get(key, 0) + 1
                if counts[key] == 3 and not injected["done"]:
                    fd = kwargs["dir_fd"]
                    original_rename("victim", "displaced", src_dir_fd=fd, dst_dir_fd=fd)
                    original_rename("foreign", "victim", src_dir_fd=fd, dst_dir_fd=fd)
                    injected["done"] = True
            return original_stat(path, *args, **kwargs)
        subject.os.stat = stat_entry
    elif point == "race_source_before_rename":
        original_rename = subject.os.rename
        def rename(source, destination, *args, **kwargs):
            if source == "scratch" and kwargs.get("src_dir_fd") is not None and kwargs.get("dst_dir_fd") is not None and not injected["done"]:
                parentfd = kwargs["src_dir_fd"]
                original_rename(source, "original-scratch", src_dir_fd=parentfd, dst_dir_fd=parentfd)
                original_rename("foreign-scratch", source, src_dir_fd=parentfd, dst_dir_fd=parentfd)
                injected["done"] = True
            return original_rename(source, destination, *args, **kwargs)
        subject.os.rename = rename
    elif point == "after_first_owned_unlink":
        original, original_listdir = subject.os.unlink, subject.os.listdir
        def listdir(fd):
            names = original_listdir(fd)
            return ["first-owned"] + [n for n in names if n != "first-owned"] if "first-owned" in names else names
        def unlink(name, *args, **kwargs):
            result = original(name, *args, **kwargs)
            if name == "first-owned": die()
            return result
        subject.os.listdir = listdir
        subject.os.unlink = unlink
    else:
        raise SystemExit("unknown test-only kill boundary")
    try:
        subject.main([str(CTL), *argv[3:]])
    except BaseException:
        if injected["done"]: raise SystemExit(2)
        raise
    if point.startswith("race_") and not injected["done"]: raise SystemExit(89)
    return True


if __name__ == "__main__":
    if run_test_kill_driver(sys.argv): raise SystemExit(0)
    unittest.main(verbosity=2)
