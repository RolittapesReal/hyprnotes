#!/usr/bin/env python3
"""Bounded ownership/reaping proof for the isolated fixture supervisor."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
DRIVER = r'''import importlib.util,json,sys
spec=importlib.util.spec_from_file_location("fixture_supervisor",sys.argv[1]);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
request=json.loads(sys.stdin.readline());mode=sys.argv[2]; original=m.append_durable;failed=False;waits=[]; launch0=m.subprocess.Popen;made=[]
class Proxy:
 def __init__(self,p,name):self.p=p;self.name=name;self.fail_once=False
 @property
 def pid(self):return self.p.pid
 def poll(self):return self.p.poll()
 def wait(self,*a,**kw):
  global failed
  waits.append(self.name)
  if mode=="wait" and self.name=="fake-compositor" and not failed:failed=True;raise OSError("injected transient wait failure")
  return self.p.wait(*a,**kw)
def launch(*a,**kw):
 name="fake-compositor" if not made else "fake-app";p=Proxy(launch0(*a,**kw),name);made.append(p);return p
def append(path,row):
 global failed
 if mode=="ledger" and row.get("event")=="graceful_marker" and not failed:failed=True;raise OSError("injected ledger write failure")
 if mode=="identity" and row.get("event")=="created" and row.get("name")=="fake-app" and not failed:failed=True;raise OSError("injected second identity publication failure")
 return original(path,row)
m.subprocess.Popen=launch;m.append_durable=append
try: print(json.dumps({"result":m.run(request),"wait_attempts":waits}))
except Exception as e:
 # Test-driver-only fallback uses known direct Popen handles and marker paths.
 # It prevents a RED run against the pre-fix helper from leaking its children.
 import os
 for p in made:
  try:
   stop=__import__("pathlib").Path(request["work"])/(p.name+".stop");stop.touch(mode=0o600,exist_ok=True);status=p.p.wait(timeout=5)
   fd=os.open(__import__("pathlib").Path(request["evidence"])/"test_driver_waits.jsonl",os.O_WRONLY|os.O_APPEND|os.O_CREAT,0o600);os.write(fd,(json.dumps({"name":p.name,"event":"wait_reaped","status":status})+"\n").encode());os.fsync(fd);os.close(fd)
  except Exception as cleanup_error:
   fd=os.open(__import__("pathlib").Path(request["evidence"])/"test_driver_waits.jsonl",os.O_WRONLY|os.O_APPEND|os.O_CREAT,0o600);os.write(fd,(json.dumps({"name":p.name,"event":"wait_uncertain","error_type":type(cleanup_error).__name__})+"\n").encode());os.fsync(fd);os.close(fd)
 print(json.dumps({"error":type(e).__name__,"wait_attempts":waits}));sys.exit(1)
'''


class FixtureSupervisorTests(unittest.TestCase):
    def make_root(self):
        root = Path(tempfile.mkdtemp(prefix="hn-fixture-supervisor-", dir="/tmp/opencode"))
        root.chmod(0o700)
        evidence, work = root / "evidence", root / "work"
        evidence.mkdir(mode=0o700)
        work.mkdir(mode=0o700)
        return root, evidence, work

    def invoke(self, evidence, work, mode=None):
        helper = HERE / "fixture_supervisor.py"
        self.assertTrue(helper.is_file(), "isolated fixture supervisor helper is missing")
        request = {"evidence": str(evidence), "work": str(work), "scenario": "assertion_failure"}
        command = ([sys.executable, str(helper)] if mode is None else
                   [sys.executable, "-c", DRIVER, str(helper), mode])
        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        try:
            try:
                stdout, stderr = proc.communicate(json.dumps(request) + "\n", timeout=40)
            except subprocess.TimeoutExpired:
                # The helper owns its children and runs bounded finally cleanup.
                # Retry waiting only on this known parent-owned Popen handle.
                stdout, stderr = proc.communicate(timeout=15)
        finally:
            if proc.poll() is None:
                # Finalize/wait only this retained helper handle. The helper's
                # own bounded finally block owns all fixture-child cleanup.
                try:
                    proc.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    raise AssertionError("retained helper handle remained uncertain; evidence kept")
            else:
                proc.wait()
        return proc.returncode, stdout, stderr

    def test_assertion_failure_still_waits_and_reaps_both_owned_children(self):
        root, evidence, work = self.make_root()
        code, stdout, stderr = self.invoke(evidence, work)
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)
        self.assertEqual(result["scenario"], "assertion_failure")
        self.assertTrue(result["simulated_assertion_failed"])
        self.assertEqual(result["children_requested"], ["fake-compositor", "fake-app"])
        self.assertEqual(result["waited_and_reaped"], ["fake-compositor", "fake-app"])
        self.assertEqual(result["remaining_children"], 0)
        self.assertFalse(work.exists(), "work directory not removed after successful reaping")
        ledger = evidence / "ownership.jsonl"
        self.assertTrue(ledger.is_file())
        self.assertEqual(ledger.stat().st_mode & 0o777, 0o600)
        rows = [json.loads(line) for line in ledger.read_text().splitlines()]
        identities = [r for r in rows if r.get("event") == "created"]
        reaped = [r for r in rows if r.get("event") == "wait_reaped"]
        self.assertEqual([r["name"] for r in identities], ["fake-compositor", "fake-app"])
        self.assertEqual([r["name"] for r in reaped], ["fake-compositor", "fake-app"])
        for row in identities:
            identity = row["identity"]
            self.assertEqual(identity["uid"], os.getuid())
            self.assertEqual(identity["exe"], os.path.realpath(sys.executable))
            self.assertTrue(identity["start"] and identity["argv"] and identity["token"])
        self.assertNotIn("token", stdout.lower())
        self.assertNotIn("token", stderr.lower())
        self.assertEqual(root.stat().st_mode & 0o777, 0o700)

    def test_transient_first_wait_error_retries_and_attempts_second_child(self):
        _, evidence, work = self.make_root()
        code, stdout, stderr = self.invoke(evidence, work, "wait")
        self.assertEqual(code, 0, stderr)
        result = json.loads(stdout)
        self.assertEqual(result["wait_attempts"], ["fake-compositor", "fake-compositor", "fake-app"])
        self.assertEqual(result["result"]["remaining_children"], 0)
        self.assertFalse(work.exists())
        errors = [json.loads(line) for line in (evidence / "errors.jsonl").read_text().splitlines()]
        self.assertTrue(any(r["event"] == "wait_retry" and r["name"] == "fake-compositor" for r in errors))

    def test_ledger_write_failure_attempts_all_waits_and_retains_work(self):
        _, evidence, work = self.make_root()
        code, stdout, stderr = self.invoke(evidence, work, "ledger")
        self.assertNotEqual(code, 0, "ledger failure must not report success")
        result = json.loads(stdout)
        self.assertEqual(result["wait_attempts"], ["fake-compositor", "fake-app"])
        self.assertTrue(work.exists(), "cleanup failure must retain work artifacts")
        rows = [json.loads(line) for line in (evidence / "ownership.jsonl").read_text().splitlines()]
        self.assertFalse(any(r.get("event") == "finalized" for r in rows))
        self.assertTrue((evidence / "errors.jsonl").is_file())
        self.assertEqual([r["name"] for r in rows if r.get("event") == "wait_reaped"],
                         ["fake-compositor", "fake-app"])

    def test_second_identity_publication_failure_never_completes_proof(self):
        _, evidence, work = self.make_root()
        code, stdout, stderr = self.invoke(evidence, work, "identity")
        self.assertNotEqual(code, 0, "missing durable identity must fail the helper")
        result = json.loads(stdout)
        self.assertEqual(result["wait_attempts"], ["fake-compositor", "fake-app"])
        rows = [json.loads(line) for line in (evidence / "ownership.jsonl").read_text().splitlines()]
        self.assertEqual([r["name"] for r in rows if r.get("event") == "created"],
                         ["fake-compositor"])
        self.assertFalse(any(r.get("event") == "proof_complete" for r in rows))
        self.assertTrue(work.exists(), "incomplete identity proof must retain work")
        self.assertEqual([r["name"] for r in rows if r.get("event") == "wait_reaped"],
                         ["fake-compositor", "fake-app"])
        errors = [json.loads(line) for line in (evidence / "errors.jsonl").read_text().splitlines()]
        self.assertTrue(any(r["event"] == "fixture_setup_failure" for r in errors))
        self.assertNotIn("token", stdout.lower())
        self.assertNotIn("token", stderr.lower())


if __name__ == "__main__":
    unittest.main(verbosity=2)
