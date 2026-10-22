#!/usr/bin/env python3
"""Isolated fixture adoption scenarios; execution requires separate approval."""
import base64
import ast
import hashlib
import inspect
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
GUARD_KEYS = {"pid", "uid", "start", "exe", "argv", "config", "token"}
BOOTSTRAP_ARG_NAMES = (
    "python_c", "here", "operation", "root", "evidence", "work", "launcher",
    "ledger_mode", "guard_mode", "proof_mode", "wait_mode", "waitpid_mode", "run_id",
)


def map_bootstrap_argv(argv):
    if len(argv) != len(BOOTSTRAP_ARG_NAMES):
        raise ValueError("bootstrap argument count mismatch")
    args = dict(zip(BOOTSTRAP_ARG_NAMES, argv))
    if args["python_c"] != "-c" or args["operation"] != "run":
        raise ValueError("bootstrap invocation mismatch")
    allowed_modes = {
        "ledger_mode": {"normal", "ledger-failure"},
        "guard_mode": {"allow", "refuse"},
        "proof_mode": {"normal", "proof-failure", "postrename-failure"},
        "wait_mode": {"normal", "wait-failure"},
        "waitpid_mode": {"normal", "eintr-once"},
    }
    if any(args[name] not in allowed for name, allowed in allowed_modes.items()):
        raise ValueError("bootstrap mode mismatch")
    run_id = args["run_id"]
    if len(run_id) != 32 or any(char not in "0123456789abcdef" for char in run_id):
        raise ValueError("bootstrap run ID mismatch")
    return args


BOOTSTRAP_SETUP = """
import sys
args = map_bootstrap_argv(sys.argv)
sys.path.insert(0, args["here"])
import fixture_chain_supervisor as m
import process_guard
from pathlib import Path
original = process_guard.signal_record
process_guard.signal_record = (lambda *a, **k: False) if args["guard_mode"] == "refuse" else original
import subprocess
original_wait = subprocess.Popen.wait
wait_count = [0]
in_popen_wait = [False]
def injected_wait(self, *a, **k):
    wait_count[0] += 1
    in_popen_wait[0] = True
    try:
        status = original_wait(self, *a, **k)
    finally:
        in_popen_wait[0] = False
    if args["wait_mode"] == "wait-failure" and wait_count[0] == 1:
        raise OSError("injected wait proof failure")
    return status
subprocess.Popen.wait = injected_wait
import os
original_waitpid = os.waitpid
waitpid_count = [0]
def injected_waitpid(pid, options):
    if args["waitpid_mode"] == "eintr-once" and pid > 0 and waitpid_count[0] == 0 and not in_popen_wait[0]:
        waitpid_count[0] += 1
        raise InterruptedError()
    return original_waitpid(pid, options)
os.waitpid = injected_waitpid
writer = m.write_ledger
count = [0]
def injected_ledger(*a, **k):
    count[0] += 1
    if count[0] == 2 and args["ledger_mode"] == "ledger-failure":
        raise OSError("injected ledger publication failure")
    return writer(*a, **k)
m.write_ledger = injected_ledger
if args["proof_mode"] == "proof-failure":
    m._publish_proof = lambda *a, **k: (_ for _ in ()).throw(OSError("injected proof publication failure"))
if args["proof_mode"] == "postrename-failure":
    original_publish = m._publish_proof
    original_sync = m._fsync_dir
    original_unlink = Path.unlink
    def uncertain_publish(path, proof):
        sync_count = [0]
        def fail_after_rename(directory):
            sync_count[0] += 1
            if sync_count[0] == 2:
                raise OSError("injected post-rename directory fsync failure")
            return original_sync(directory)
        def refuse_invalidation(self, *a, **k):
            if self == path:
                raise OSError("injected proof invalidation failure")
            return original_unlink(self, *a, **k)
        m._fsync_dir = fail_after_rename
        Path.unlink = refuse_invalidation
        try:
            return original_publish(path, proof)
        finally:
            m._fsync_dir = original_sync
            Path.unlink = original_unlink
    m._publish_proof = uncertain_publish
raise SystemExit(m.run(args["root"], args["evidence"], args["work"], args["launcher"],
                       ledger_writer=injected_ledger, run_id=args["run_id"]))
"""


def consume_result(returncode, stdout, proof_path, expected_run_id):
    """Accept success only with process outcome, durable receipt, digest, and exact waits."""
    if returncode != 0 or not isinstance(stdout, str):
        return False
    lines = stdout.splitlines()
    if len(lines) != 1:
        return False
    fields = lines[0].split(" ")
    if len(fields) != 3 or fields[0] != "FIXTURE_PROOF_ACK" or fields[1] != expected_run_id:
        return False
    try:
        st = proof_path.lstat()
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o600:
            return False
        raw = proof_path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != fields[2]:
            return False
        proof = json.loads(raw)
    except (OSError, ValueError, TypeError):
        return False
    if not isinstance(proof, dict):
        return False
    proof_keys = {"schema", "proof_complete", "primary_error", "run_id", "cleanup_errors",
                  "ownership", "waits", "wait_retries", "driver", "registered", "ledger_rows"}
    if set(proof) != proof_keys or (proof.get("schema") != 2 or proof.get("run_id") != expected_run_id
            or proof.get("proof_complete") is not True or proof.get("primary_error") is not None
            or proof.get("cleanup_errors") != [] or proof.get("ledger_rows") != 3):
        return False
    driver = proof.get("driver")
    registered = proof.get("registered")
    if not isinstance(driver, dict) or set(driver) != GUARD_KEYS or not isinstance(registered, list) or len(registered) != 1:
        return False
    app_row = registered[0]
    if not isinstance(app_row, dict) or set(app_row) != {"guard", "history"}:
        return False
    app, history = app_row["guard"], app_row["history"]
    if not isinstance(app, dict) or set(app) != GUARD_KEYS or not isinstance(history, dict):
        return False
    for identity in (driver, app):
        if (type(identity.get("pid")) is not int or identity["pid"] <= 0
                or type(identity.get("uid")) is not int or not isinstance(identity.get("start"), str)
                or not isinstance(identity.get("exe"), str) or not isinstance(identity.get("config"), str)
                or not isinstance(identity.get("token"), str)):
            return False
        try:
            if not base64.b64decode(identity["argv"], validate=True):
                return False
        except (ValueError, TypeError):
            return False
    if app["token"] != driver["token"] or app["config"] != driver["config"]:
        return False
    if (set(history) != {"role", "creator", "creator_parent", "parent"}
            or history.get("role") != "app" or history.get("creator") != driver
            or history.get("parent") != {"parent_pid": driver.get("pid"), "parent_start": driver.get("start")}):
        return False
    owner_parent = history.get("creator_parent")
    if (not isinstance(owner_parent, dict) or set(owner_parent) != {"parent_pid", "parent_start"}
            or type(owner_parent.get("parent_pid")) is not int or not isinstance(owner_parent.get("parent_start"), str)):
        return False
    phases = proof.get("ownership")
    phase_keys = {"driver_created", "driver_identity_committed", "app_seen", "app_identity_committed",
                  "ack_sent", "handoff_confirmed", "app_wait_attempted", "app_reaped"}
    if not isinstance(phases, dict) or set(phases) != phase_keys or any(value is not True for value in phases.values()):
        return False
    waits = proof.get("waits")
    expected = {
        ("driver", "supervisor", "Popen.wait", driver.get("pid"), driver.get("start"), 0, True),
        ("app", "supervisor-adopter", "waitpid", app.get("pid"), app.get("start"), 0, True),
    }
    if not isinstance(waits, list) or len(waits) != 2:
        return False
    actual = set()
    for row in waits:
        if not isinstance(row, dict) or set(row) != {"role", "owner", "wait_api", "pid", "start", "status", "actual"}:
            return False
        actual.add((row["role"], row["owner"], row["wait_api"], row["pid"], row["start"], row["status"], row["actual"]))
    retries = proof.get("wait_retries")
    if not isinstance(retries, list):
        return False
    for retry in retries:
        if (not isinstance(retry, dict) or set(retry) != {"role", "pid", "attempt", "reason"}
                or retry.get("role") not in {"app", "app-cleanup", "app-retained-owner"}
                or retry.get("pid") != app.get("pid") or retry.get("reason") != "EINTR"
                or type(retry.get("attempt")) is not int or retry["attempt"] < 1):
            return False
    return actual == expected


class FixtureChainSupervisorTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="hn-adopt-"))
        os.chmod(self.root, 0o700)
        self.evidence = self.root / "evidence"
        self.evidence.mkdir(mode=0o700)
        self.work = self.root / "work"
        self.work.mkdir(mode=0o700)
        self.children = []
        self.run_id = os.urandom(16).hex()

    def tearDown(self):
        # Keep every root and every proof artifact. Never terminate on timeout.
        for child in self.children:
            if child.poll() is None:
                self.fail("owner still active; preserve exact root " + str(self.root))

    def run_case(self, scenario="happy"):
        launcher, app = self.root / "fixture-driver.py", self.root / "fixture-app.py"
        app.write_text(
            "import pathlib,signal,sys,time\n"
            "p=pathlib.Path(sys.argv[1]); p.write_text('READY')\n"
            "def stop(*_): p.with_name('stopped').write_text('TERM'); raise SystemExit(0)\n"
            "signal.signal(signal.SIGTERM, stop)\n"
            "while not p.with_name('stop-request').exists(): time.sleep(.01)\n"
            "p.with_name('stopped').write_text('marker'); raise SystemExit(0)\n")
        app.chmod(0o700)
        launcher.write_text(
            "import base64,json,os,pathlib,socket,stat,sys,time\n"
            "sys.path.insert(0,sys.argv[5]); import process_guard\n"
            "sockpath,token,app,root,guarddir=sys.argv[1:]\n"
            "child=None; handed_off=False; sock=None; stream=None; identity=None; child_start=None; child_parent=None\n"
            "def durable_wait(status):\n"
            " record={'schema':1,'owner':'creator','wait_api':'Popen.wait','pid':child.pid,'start':child_start,'status':status,'actual':True,'creator_pid':os.getpid(),'identity':identity,'parent_pid':child_parent[0],'parent_start':child_parent[1]}\n"
            " tmp=root+'/.creator-wait.tmp'; final=root+'/creator-wait.json'\n"
            " fd=os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_CLOEXEC|os.O_NOFOLLOW,0o600)\n"
            " with os.fdopen(fd,'w') as f: f.write(json.dumps(record,sort_keys=True)+'\\n'); f.flush(); os.fsync(f.fileno())\n"
            " os.replace(tmp,final); d=os.open(root,os.O_RDONLY|os.O_DIRECTORY); os.fsync(d); os.close(d)\n"
            "try:\n"
            " child=__import__('subprocess').Popen([sys.executable,app,root+'/app-ready'],env=dict(os.environ,HN_LIVE_OWNER_TOKEN=token))\n"
            " fields=open('/proc/%d/stat'%child.pid).read().rsplit(')',1)[1].split(); child_start=fields[19]; self_fields=open('/proc/self/stat').read().rsplit(')',1)[1].split(); child_parent=(int(fields[1]), self_fields[19]); identity=process_guard.snapshot(child.pid,app,token)\n"
            " deadline=time.monotonic()+4\n"
            " while not os.path.exists(root+'/app-ready') and time.monotonic()<deadline: time.sleep(.01)\n"
            " if not os.path.exists(root+'/app-ready'): raise TimeoutError('fixture readiness deadline')\n"
            " if os.path.exists(root+'/pre-registration-failure'): raise ValueError('injected pre-registration failure')\n"
            " sock=socket.socket(socket.AF_UNIX); sock.settimeout(5); sock.connect(sockpath); stream=sock.makefile('rb')\n"
            " sock.sendall((json.dumps({'op':'register','pid':child.pid,'token':token})+'\\n').encode())\n"
            " reply=stream.readline(8192)\n"
            " if reply != b'ACK identity committed\\n': raise ValueError('registration rejected or EOF')\n"
            " sock.sendall(b'HANDOFF\\n')\n"
            " if stream.readline(8192) != b'ADOPTED\\n': raise ValueError('handoff not adopted')\n"
            " sock.sendall(b'RELEASED\\n')\n"
            " if stream.readline(8192) != b'TRANSFERRED\\n': raise ValueError('transfer not confirmed')\n"
            " handed_off=True\n"
            "except BaseException:\n"
            " pass\n"
            "finally:\n"
            " if stream is not None: stream.close()\n"
            " if sock is not None: sock.close()\n"
            " if child is not None and not handed_off:\n"
            "  pathlib.Path(root+'/stop-request').write_text('fixture cleanup\\n')\n"
            "  status=child.wait()\n"
            "  durable_wait(status)\n")
        launcher.chmod(0o700)
        if scenario == "prereg":
            (self.root / "pre-registration-failure").write_text("inject\n")
        ledger_mode = "ledger-failure" if scenario == "ledger-failure" else "normal"
        guard_mode = "refuse" if scenario == "guard-refusal" else "allow"
        proof_mode = "proof-failure" if scenario == "proof-failure" else "normal"
        wait_mode = "wait-failure" if scenario == "wait-failure" else "normal"
        if scenario == "postrename-failure":
            proof_mode = "postrename-failure"
        waitpid_mode = "eintr-once" if scenario == "eintr" else "normal"
        argv = ["-c", str(HERE), "run", str(self.root), str(self.evidence), str(self.work),
                str(launcher), ledger_mode, guard_mode, proof_mode, wait_mode, waitpid_mode, self.run_id]
        args = map_bootstrap_argv(argv)
        bootstrap = ("BOOTSTRAP_ARG_NAMES = " + repr(BOOTSTRAP_ARG_NAMES) + "\n"
                     + inspect.getsource(map_bootstrap_argv) + BOOTSTRAP_SETUP)
        cmd = [sys.executable, args["python_c"], bootstrap,
               *(args[name] for name in BOOTSTRAP_ARG_NAMES[1:])]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(proc)
        try:
            out, err = proc.communicate(timeout=16)
        except subprocess.TimeoutExpired:
            self.fail("owner exceeded bound; no terminate; preserve " + str(self.root))
        proof_path = self.evidence / "proof.json"
        proof = json.loads(proof_path.read_text()) if proof_path.exists() else None
        return proc.returncode, proof, out, err

    def test_registered_descendant_adoption_and_exact_actual_waits(self):
        code, proof, out, err = self.run_case()
        self.assertEqual(code, 0, out + err)
        self.assertTrue(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        digest = hashlib.sha256((self.evidence / "proof.json").read_bytes()).hexdigest()
        self.assertFalse(consume_result(code, f"FIXTURE_PROOF_ACK {self.run_id} {'0' * 64}\n",
                                        self.evidence / "proof.json", self.run_id))
        self.assertTrue(consume_result(code, f"FIXTURE_PROOF_ACK {self.run_id} {digest}\n",
                                       self.evidence / "proof.json", self.run_id))
        self.assertTrue(proof["proof_complete"])
        self.assertEqual({(w["role"], w["owner"], w["wait_api"], w["status"])
                          for w in proof["waits"]},
                         {("driver", "supervisor", "Popen.wait", 0),
                          ("app", "supervisor-adopter", "waitpid", 0)})
        self.assertTrue((self.root / "stopped").is_file())
        self.assertTrue(self.work.exists(), "work/root retained by explicit source ruling")

    def test_second_identity_publication_failure_nacks_and_creator_waits(self):
        code, proof, out, err = self.run_case("ledger-failure")
        self.assertNotEqual(code, 0, out + err)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertFalse(proof["proof_complete"])
        self.assertTrue(any(w.get("owner") == "creator" and w.get("actual") for w in proof["waits"]))
        self.assertTrue((self.root / "creator-wait.json").is_file())
        self.assertTrue(self.work.exists())

    def test_guard_refusal_uses_fake_marker_but_never_completes_proof(self):
        code, proof, out, err = self.run_case("guard-refusal")
        self.assertNotEqual(code, 0, out + err)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertFalse(proof["proof_complete"])
        self.assertTrue(proof["primary_error"])
        self.assertTrue((self.root / "stopped").is_file())
        self.assertTrue(self.work.exists())

    def test_pre_registration_failure_creator_still_waits_child(self):
        code, proof, out, err = self.run_case("prereg")
        self.assertNotEqual(code, 0, out + err)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertFalse(proof["proof_complete"])
        self.assertTrue(any(w.get("owner") == "creator" and w.get("actual") for w in proof["waits"]))
        self.assertTrue(self.work.exists())

    def test_proof_publication_failure_never_returns_success(self):
        code, proof, out, err = self.run_case("proof-failure")
        self.assertNotEqual(code, 0, out + err)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertFalse(proof and proof["proof_complete"])
        self.assertTrue(self.work.exists())

    def test_driver_wait_proof_failure_never_double_waits_or_completes(self):
        code, proof, out, err = self.run_case("wait-failure")
        self.assertNotEqual(code, 0, out + err)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertFalse(proof["proof_complete"])
        driver_waits = [w for w in proof["waits"] if w.get("role") == "driver"]
        self.assertEqual(len(driver_waits), 0, "raising after actual Popen.wait cannot fabricate wait evidence")
        self.assertTrue(any(w.get("role") == "app" and w.get("owner") == "supervisor-adopter"
                            and w.get("actual") for w in proof["waits"]))
        self.assertTrue(self.work.exists())

    def test_interrupted_waitpid_retries_and_receipt_accepts_exact_proof(self):
        code, proof, out, err = self.run_case("eintr")
        self.assertEqual(code, 0, out + err)
        self.assertTrue(consume_result(code, out, self.evidence / "proof.json", self.run_id))
        self.assertEqual(proof["wait_retries"], [{"role": "app", "pid": proof["registered"][0]["guard"]["pid"],
                                                   "attempt": 1, "reason": "EINTR"}])

    def test_visible_complete_candidate_is_denied_without_durable_receipt(self):
        code, proof, out, err = self.run_case("postrename-failure")
        self.assertEqual(code, 2, out + err)
        self.assertIsNotNone(proof)
        self.assertTrue(proof["proof_complete"], "fault leaves stale candidate visible by design")
        self.assertFalse(any(line.startswith("FIXTURE_PROOF_ACK ") for line in out.splitlines()))
        candidate_digest = hashlib.sha256((self.evidence / "proof.json").read_bytes()).hexdigest()
        self.assertNotIn(f"FIXTURE_PROOF_ACK {self.run_id} {candidate_digest}", out)
        self.assertFalse(consume_result(code, out, self.evidence / "proof.json", self.run_id))


class ProcStatParserPureTest(unittest.TestCase):
    """Run extracted parser AST against captured stat text; never import supervisor."""

    def test_parent_start_is_parent_own_start_and_child_start_stays_distinct(self):
        supervisor_pid, driver_pid, app_pid, other_pid = 41001, 41002, 41003, 41004
        starts = {supervisor_pid: "22444820", driver_pid: "22444823",
                  app_pid: "22444825", other_pid: "22444827"}

        def stat_text(pid, ppid, start):
            tail = ["S", str(ppid), *(["0"] * 17), start]
            return f"{pid} (captured fixture) " + " ".join(tail)

        stat_by_pid = {
            supervisor_pid: stat_text(supervisor_pid, 1, starts[supervisor_pid]),
            driver_pid: stat_text(driver_pid, supervisor_pid, starts[driver_pid]),
            app_pid: stat_text(app_pid, driver_pid, starts[app_pid]),
            other_pid: stat_text(other_pid, other_pid, starts[other_pid]),
        }

        class FakePath:
            def __init__(self, value):
                self.value = str(value)

            def read_text(self):
                prefix, pid_text, suffix = self.value.split("/")[-3:]
                if prefix != "proc" or suffix != "stat":
                    raise AssertionError("unexpected parser path")
                pid = int(pid_text)
                if pid not in stat_by_pid:
                    raise FileNotFoundError(self.value)
                return stat_by_pid[pid]

        def extracted(source_path, names, text=None):
            source = text if text is not None else source_path.read_text()
            tree = ast.parse(source)
            functions = [node for node in tree.body
                         if isinstance(node, ast.FunctionDef) and node.name in names]
            self.assertEqual({node.name for node in functions}, set(names))
            namespace = {"Path": FakePath}
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(source_path), "exec"),
                 namespace)
            return namespace

        # Historical defective parser, embedded so this test needs no ignored baseline archive.
        historical = (
            "def proc_parent(pid):\n"
            "    fields = Path(f\"/proc/{pid}/stat\").read_text().rsplit(\")\", 1)[1].split()\n"
            "    return int(fields[1]), fields[19]\n")
        old = extracted(HERE / "fixture_chain_supervisor.py", {"proc_parent"}, text=historical)
        # RED witness: historical parser reports driver's own start as parent's.
        self.assertNotEqual(old["proc_parent"](driver_pid),
                            (supervisor_pid, starts[supervisor_pid]))

        current = extracted(HERE / "fixture_chain_supervisor.py",
                            {"_proc_stat_fields", "proc_start", "proc_parent"})
        self.assertEqual(current["proc_start"](supervisor_pid), starts[supervisor_pid])
        self.assertEqual(current["proc_start"](driver_pid), starts[driver_pid])
        self.assertEqual(current["proc_start"](app_pid), starts[app_pid])
        self.assertEqual(current["proc_parent"](driver_pid),
                         (supervisor_pid, starts[supervisor_pid]))
        self.assertEqual(current["proc_parent"](app_pid),
                         (driver_pid, starts[driver_pid]))
        changed_relation = stat_text(driver_pid, other_pid, starts[driver_pid])
        stat_by_pid[driver_pid] = changed_relation
        observed = current["proc_parent"](driver_pid)
        self.assertNotEqual(observed, (supervisor_pid, starts[supervisor_pid]))
        stat_by_pid[driver_pid] = stat_text(driver_pid, supervisor_pid, starts[driver_pid])
        stat_by_pid[driver_pid] = stat_text(driver_pid, 49999, starts[driver_pid])
        with self.assertRaises(FileNotFoundError):
            current["proc_parent"](driver_pid)


if __name__ == "__main__":
    unittest.main(verbosity=2)
