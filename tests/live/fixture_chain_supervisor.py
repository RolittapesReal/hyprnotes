#!/usr/bin/env python3
"""Test-only owner for a fixture driver and one historically registered app."""
import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import time

import process_guard

LIMIT = 6.0
GUARD_KEYS = ("pid", "uid", "start", "exe", "argv", "config", "token")


def guard_identity(snapshot):
    return {key: snapshot[key] for key in GUARD_KEYS}


def _private_stat(path, mode, *, directory=False):
    st = os.lstat(path)
    if directory:
        valid_type = stat.S_ISDIR(st.st_mode)
    else:
        valid_type = stat.S_ISREG(st.st_mode)
    if (not valid_type or st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != mode):
        raise ValueError("private path type/owner/mode mismatch")
    return st


def _fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_new_private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC |
                 getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        if os.fstat(fd).st_uid != os.getuid() or stat.S_IMODE(os.fstat(fd).st_mode) != 0o600:
            raise ValueError("private file owner/mode mismatch")
        view = memoryview(data)
        while view:
            count = os.write(fd, view)
            if count <= 0:
                raise OSError("short private write")
            view = view[count:]
        os.fsync(fd)
    finally:
        os.close(fd)
    _fsync_dir(Path(path).parent)


def write_ledger(path, rows):
    data = b"".join((json.dumps(row, sort_keys=True) + "\n").encode() for row in rows)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC |
                 getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        st = os.fstat(fd)
        if st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o600 or not stat.S_ISREG(st.st_mode):
            raise ValueError("private ledger owner/mode/type mismatch")
        view = memoryview(data)
        while view:
            count = os.write(fd, view)
            if count <= 0:
                raise OSError("short ledger write")
            view = view[count:]
        os.fsync(fd)
    finally:
        os.close(fd)
    _fsync_dir(Path(path).parent)


def _proc_stat_fields(pid):
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()


def proc_start(pid):
    return _proc_stat_fields(pid)[19]


def proc_parent(pid):
    fields = _proc_stat_fields(pid)
    parent_pid = int(fields[1])
    return parent_pid, proc_start(parent_pid)


def _proc_identity(pid, expected_parent_pid, expected_parent_start, expected, config, token):
    raw = process_guard.snapshot(pid, config, token)
    if guard_identity(raw) != raw:
        raise ValueError("guard snapshot schema mismatch")
    if raw["uid"] != os.getuid() or raw["exe"] != os.path.realpath(sys.executable):
        raise ValueError("fixture executable/UID mismatch")
    if base64.b64decode(raw["argv"]).split(b"\0")[:-1] != [os.fsencode(a) for a in expected]:
        raise ValueError("fixture exact argv mismatch")
    parent_pid, parent_start = proc_parent(pid)
    if parent_pid != expected_parent_pid or parent_start != expected_parent_start:
        raise ValueError("fixture parent PID/start mismatch")
    return raw, {"parent_pid": parent_pid, "parent_start": parent_start}


def _publish_proof(path, proof):
    tmp = path.parent / (".proof-" + os.urandom(12).hex() + ".tmp")
    data = (json.dumps(proof, sort_keys=True) + "\n").encode()
    try:
        _write_new_private(tmp, data)
        os.replace(tmp, path)
        _fsync_dir(path.parent)
        _private_stat(path, 0o600)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        # A directory-fsync error after rename has uncertain durability. Remove a
        # visible success artifact best-effort; caller still returns failure.
        if path.exists():
            try:
                path.unlink()
                _fsync_dir(path.parent)
            except OSError:
                pass
        raise


def _load_creator_wait(path, app_guard, creator_guard, config, token):
    _private_stat(path, 0o600)
    record = json.loads(path.read_text())
    expected_keys = {"schema", "owner", "wait_api", "pid", "start", "status", "actual",
                     "creator_pid", "identity", "parent_pid", "parent_start"}
    if set(record) != expected_keys or record.get("schema") != 1 or record.get("owner") != "creator":
        raise ValueError("creator wait schema mismatch")
    if record.get("wait_api") != "Popen.wait" or record.get("actual") is not True:
        raise ValueError("creator wait lacks actual Popen.wait")
    identity = record.get("identity")
    if record.get("pid") <= 0 or not isinstance(record.get("start"), str) or not record["start"]:
        raise ValueError("creator wait identity mismatch")
    if identity is None:
        if app_guard is not None:
            raise ValueError("creator wait omitted committed identity")
    else:
        if not isinstance(identity, dict) or guard_identity(identity) != identity:
            raise ValueError("creator wait lacks exact guard identity")
        if record.get("pid") != identity.get("pid") or record.get("start") != identity.get("start"):
            raise ValueError("creator wait identity mismatch")
        if (identity.get("uid") != os.getuid() or identity.get("exe") != os.path.realpath(sys.executable)
                or identity.get("config") != os.path.realpath(config) or identity.get("token") != token):
            raise ValueError("creator wait fixture identity mismatch")
        expected_argv = [sys.executable, os.path.realpath(config), str(Path(config).parent / "app-ready")]
        if base64.b64decode(identity["argv"]).split(b"\0")[:-1] != [os.fsencode(a) for a in expected_argv]:
            raise ValueError("creator wait exact argv mismatch")
    if (record.get("parent_pid"), record.get("parent_start")) != (
            creator_guard["pid"], creator_guard["start"]):
        raise ValueError("creator wait parent relation mismatch")
    if app_guard is not None and identity != app_guard:
        raise ValueError("creator wait differs from committed identity")
    if type(record.get("status")) is not int or record.get("status") != 0:
        raise ValueError("creator wait reports unsuccessful app exit")
    if record.get("creator_pid") != creator_guard["pid"]:
        raise ValueError("creator wait owner mismatch")
    return {key: record[key] for key in expected_keys}


def run(root, evidence, work, launcher, ledger_writer=write_ledger, run_id=None):
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("root must be an existing non-symlink private directory")
    _private_stat(root, 0o700, directory=True)
    root = root.resolve(strict=True)
    evidence, work = Path(evidence), Path(work)
    for path in (evidence, work):
        if path.is_symlink() or path.parent.resolve(strict=True) != root:
            raise ValueError("evidence/work must be direct non-symlink root children")
        _private_stat(path, 0o700, directory=True)
    evidence, work = evidence.resolve(strict=True), work.resolve(strict=True)
    launcher = Path(launcher)
    if launcher.is_symlink() or launcher.resolve(strict=True).parent != root or not launcher.is_file():
        raise ValueError("launcher must be test-created direct child of private root")

    ledger, proof_path = evidence / "ownership.jsonl", evidence / "proof.json"
    sock_path, creator_wait_path = root / "c.sock", root / "creator-wait.json"
    for path in (ledger, proof_path, sock_path, creator_wait_path, root / "stop-request"):
        if path.exists() or path.is_symlink():
            raise ValueError("unexpected pre-existing fixture artifact")

    token = os.urandom(24).hex()
    run_id = run_id or os.urandom(16).hex()
    if not re.fullmatch(r"[0-9a-f]{32}", run_id):
        raise ValueError("invalid nonsecret invocation ID")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(sock_path))
    os.chmod(sock_path, 0o600)
    listener.listen(1)
    listener.settimeout(LIMIT)
    proc = conn = None
    driver_guard = app_guard = app_meta = None
    rows, waits, cleanup_errors, wait_retries = [], [], [], []
    ledger_row_count = 0
    primary_error = None
    phases = {"driver_created": False, "driver_identity_committed": False,
              "app_seen": False, "app_identity_committed": False,
              "ack_sent": False, "handoff_confirmed": False, "app_wait_attempted": False,
              "app_reaped": False}
    env = dict(os.environ, HN_LIVE_OWNER_TOKEN=token)
    config = str(root / "fixture-app.py")
    proof = {"schema": 2, "proof_complete": False, "primary_error": None,
             "run_id": run_id, "cleanup_errors": cleanup_errors, "ownership": phases,
             "waits": waits, "wait_retries": wait_retries}

    def waitpid_exact(pid, role):
        attempt = 0
        while True:
            attempt += 1
            try:
                return os.waitpid(pid, 0)
            except InterruptedError:
                wait_retries.append({"role": role, "pid": pid, "attempt": attempt, "reason": "EINTR"})

    def fail(where, exc):
        nonlocal primary_error
        if primary_error is None:
            # Never put exception text, argv, token, or environment into proof.
            primary_error = {"where": where, "type": type(exc).__name__}
        else:
            cleanup_errors.append({"where": where, "type": type(exc).__name__})

    def cleanup_failure(where, exc):
        cleanup_errors.append({"where": where, "type": type(exc).__name__})

    def send_nack(reason):
        if conn is None:
            return
        try:
            conn.sendall(("NACK " + reason + "\n").encode())
        except OSError as exc:
            cleanup_failure("nack-write", exc)

    try:
        if ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) != 0:
            raise OSError("subreaper setup failed")
        guard_dir = str(Path(process_guard.__file__).resolve().parent)
        proc = subprocess.Popen([sys.executable, str(launcher.resolve()), str(sock_path), token,
                                 config, str(root), guard_dir], cwd=root, env=env)
        phases["driver_created"] = True
        driver_argv = [sys.executable, str(launcher.resolve()), str(sock_path), token,
                       config, str(root), guard_dir]
        supervisor_start = proc_start(os.getpid())
        driver_guard, driver_parent = _proc_identity(proc.pid, os.getpid(), supervisor_start,
                                                      driver_argv, config, token)
        driver_row = {"guard": driver_guard, "role": "driver", "parent": driver_parent,
                      "phase": "created"}
        rows.append(driver_row)
        ledger_writer(ledger, [driver_row])
        ledger_row_count += 1
        phases["driver_identity_committed"] = True
        conn, _ = listener.accept()
        conn.settimeout(LIMIT)
        stream = conn.makefile("rb")
        raw = stream.readline(8192)
        if not raw.endswith(b"\n"):
            raise ValueError("registration EOF or overlong request")
        request = json.loads(raw)
        if set(request) != {"op", "pid", "token"} or request["op"] != "register" or request["token"] != token:
            raise ValueError("registration schema/authentication mismatch")
        pid = int(request["pid"])
        phases["app_seen"] = True
        app_argv = [sys.executable, config, str(root / "app-ready")]
        app_guard, app_parent = _proc_identity(pid, proc.pid, driver_guard["start"],
                                                app_argv, config, token)
        app_meta = {"role": "app", "creator": driver_guard,
                    "creator_parent": driver_parent, "parent": app_parent}
        if app_guard["uid"] != driver_guard["uid"] or app_guard["exe"] != driver_guard["exe"]:
            raise ValueError("app/creator UID or executable relation mismatch")
        row = {"guard": app_guard, "history": app_meta, "phase": "identity-committed"}
        rows.append(row)
        ledger_writer(ledger, [row])
        ledger_row_count += 1
        phases["app_identity_committed"] = True
        conn.sendall(b"ACK identity committed\n")
        phases["ack_sent"] = True
        response = stream.readline(8192)
        if response != b"HANDOFF\n":
            raise ValueError("driver did not confirm handoff")
        conn.sendall(b"ADOPTED\n")
        done = stream.readline(8192)
        if done != b"RELEASED\n":
            raise ValueError("driver did not confirm release")
        conn.sendall(b"TRANSFERRED\n")
        phases["handoff_confirmed"] = True
        driver_status = proc.wait(timeout=LIMIT)
        waits.append({"role": "driver", "owner": "supervisor", "wait_api": "Popen.wait",
                      "pid": driver_guard["pid"], "start": driver_guard["start"],
                      "status": driver_status, "actual": True})
        if driver_status != 0:
            raise RuntimeError("fixture driver returned failure")
        ppid, pstart = proc_parent(pid)
        if ppid != os.getpid() or process_guard.snapshot(pid, config, token) != app_guard:
            raise ValueError("app not adopted with historical identity")
        if (ppid, pstart) != (os.getpid(), proc_start(os.getpid())):
            raise ValueError("adopted app parent PID/start mismatch")
        if not process_guard.signal_record(app_guard, config):
            raise RuntimeError("strict guard refused app stop")
        try:
            phases["app_wait_attempted"] = True
            waited_pid, wait_status = waitpid_exact(pid, "app")
            if waited_pid != pid:
                raise ChildProcessError("waitpid returned unexpected PID")
            phases["app_reaped"] = True
            waits.append({"role": "app", "owner": "supervisor-adopter", "wait_api": "waitpid",
                          "pid": app_guard["pid"], "start": app_guard["start"],
                          "status": wait_status, "actual": True})
            row["phase"] = "adopter-waited"
            ledger_writer(ledger, [{"guard": app_guard, "history": app_meta,
                                    "phase": "adopter-waited", "waitpid_status": wait_status}])
            ledger_row_count += 1
        except BaseException:
            # No second wait attempt can convert an already-returned wait error into proof.
            raise
    except BaseException as exc:
        fail("supervisor", exc)
        if conn is not None and not phases["handoff_confirmed"]:
            send_nack("cleanup required")
        # Before confirmed handoff, creator remains cleanup owner. Marker requests
        # cleanup only from fixture-created fake app; supervisor never signals it.
    finally:
        if conn is not None:
            try:
                conn.close()
            except OSError as exc:
                cleanup_failure("connection-close", exc)
        try:
            listener.close()
        except OSError as exc:
            cleanup_failure("listener-close", exc)

        creator_record_loaded = False

        def collect_creator_wait():
            nonlocal creator_record_loaded
            if driver_guard is None or creator_record_loaded or not creator_wait_path.exists():
                return
            try:
                waits.append({"role": "app", "owner": "creator",
                              **_load_creator_wait(creator_wait_path, app_guard, driver_guard, config, token)})
                creator_record_loaded = True
            except BaseException as exc:
                cleanup_failure("creator-wait-record", exc)

        # Try independent durable creator evidence before blocking on driver handle.
        collect_creator_wait()
        if proc is not None and proc.returncode is None:
            try:
                status = proc.wait(timeout=LIMIT)
                waits.append({"role": "driver", "owner": "supervisor", "wait_api": "Popen.wait",
                              "pid": driver_guard["pid"] if driver_guard else proc.pid,
                              "start": driver_guard["start"] if driver_guard else None,
                              "status": status, "actual": True})
            except subprocess.TimeoutExpired as exc:
                cleanup_failure("driver-wait-deadline", exc)
                # Keep direct-handle ownership alive rather than return with a
                # potentially running known child orphaned from this supervisor.
                try:
                    status = proc.wait()
                    waits.append({"role": "driver", "owner": "supervisor", "wait_api": "Popen.wait",
                                  "pid": driver_guard["pid"] if driver_guard else proc.pid,
                                  "start": driver_guard["start"] if driver_guard else None,
                                  "status": status, "actual": True})
                except BaseException as retry_exc:
                    cleanup_failure("driver-wait-retained-owner", retry_exc)
            except BaseException as exc:
                cleanup_failure("driver-wait", exc)

        # Recheck after driver wait: driver may have durably published its wait row
        # while Popen.wait was outstanding. Creator proof never substitutes for adopter wait.
        collect_creator_wait()

        # A confirmed handoff may leave app as our child even after a later error.
        # Only committed historical raw identity can authorize guarded signal/wait.
        app_wait_exists = any(w.get("role") == "app" and w.get("owner") == "supervisor-adopter"
                              and w.get("actual") for w in waits)
        creator_wait_exists = any(w.get("role") == "app" and w.get("owner") == "creator"
                                  and w.get("actual") is True for w in waits)
        if phases["handoff_confirmed"] and creator_wait_exists:
            cleanup_failure("handoff-uncertain", RuntimeError("creator retained and waited app"))
        if (phases["handoff_confirmed"] and app_guard is not None and not app_wait_exists
                and not creator_wait_exists and not phases["app_reaped"]):
            try:
                ppid, _ = proc_parent(app_guard["pid"])
                if ppid == os.getpid():
                    if not process_guard.signal_record(app_guard, config):
                        cleanup_failure("adopted-app-guard", RuntimeError("strict guard refused cleanup signal"))
                        _write_new_private(root / "stop-request", b"fixture cleanup\n")
                    phases["app_wait_attempted"] = True
                    waited_pid, status = waitpid_exact(app_guard["pid"], "app-cleanup")
                    if waited_pid != app_guard["pid"]:
                        raise ChildProcessError("cleanup waitpid returned unexpected PID")
                    phases["app_reaped"] = True
                    waits.append({"role": "app", "owner": "supervisor-adopter", "wait_api": "waitpid",
                                  "pid": app_guard["pid"], "start": app_guard["start"],
                                  "status": status, "actual": True})
                else:
                    raise RuntimeError("adopted app parent is no longer supervisor")
            except BaseException as exc:
                cleanup_failure("adopted-app-cleanup", exc)
                # Ownership remains with this live supervisor. An indeterminate
                # wait result is not permission to return with a possibly live child.
                while not phases["app_reaped"]:
                    try:
                        waited_pid, status = waitpid_exact(app_guard["pid"], "app-retained-owner")
                        if waited_pid == app_guard["pid"]:
                            phases["app_reaped"] = True
                            waits.append({"role": "app", "owner": "supervisor-adopter",
                                          "wait_api": "waitpid", "pid": app_guard["pid"],
                                          "start": app_guard["start"], "status": status, "actual": True})
                    except BaseException:
                        time.sleep(0.05)

        try:
            if sock_path.exists() or sock_path.is_symlink():
                st = os.lstat(sock_path)
                if not stat.S_ISSOCK(st.st_mode) or st.st_uid != os.getuid():
                    raise ValueError("socket ownership/type changed")
                sock_path.unlink()
                _fsync_dir(root)
        except BaseException as exc:
            cleanup_failure("socket-remove", exc)

        proof["driver"] = driver_guard
        proof["registered"] = ([{"guard": app_guard, "history": app_meta}] if app_guard else [])
        proof["ledger_rows"] = ledger_row_count
        expected_driver = driver_guard is not None and sum(
            1 for w in waits if w.get("role") == "driver" and w.get("owner") == "supervisor"
            and w.get("wait_api") == "Popen.wait" and w.get("actual") is True
            and w.get("pid") == driver_guard["pid"] and w.get("start") == driver_guard["start"]
            and w.get("status") == 0) == 1
        expected_app = app_guard is not None and sum(
            1 for w in waits if w.get("role") == "app" and w.get("owner") == "supervisor-adopter"
            and w.get("wait_api") == "waitpid" and w.get("actual") is True
            and w.get("pid") == app_guard["pid"] and w.get("start") == app_guard["start"]
            and w.get("status") == 0) == 1
        expected_wait_set = len(waits) == 2
        proof["proof_complete"] = bool(expected_wait_set and phases["handoff_confirmed"] and primary_error is None
                                       and not cleanup_errors and phases["driver_identity_committed"]
                                       and phases["app_identity_committed"] and expected_driver and expected_app)
        proof["primary_error"] = primary_error
        proof["cleanup_errors"] = cleanup_errors
        if proof["proof_complete"]:
            try:
                _publish_proof(proof_path, proof)
                _private_stat(proof_path, 0o600)
                digest = hashlib.sha256(proof_path.read_bytes()).hexdigest()
                sys.stdout.write(f"FIXTURE_PROOF_ACK {run_id} {digest}\n")
                sys.stdout.flush()
            except BaseException as exc:
                fail("proof-publish", exc)
                proof["proof_complete"] = False
        else:
            try:
                _publish_proof(proof_path, proof)
            except BaseException as exc:
                cleanup_failure("failure-proof-publish", exc)
    return 0 if proof["proof_complete"] else 2


def main(argv):
    if len(argv) != 5 or argv[0] != "run":
        raise SystemExit("usage: fixture_chain_supervisor.py run ROOT EVIDENCE WORK LAUNCHER")
    return run(*argv[1:])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
