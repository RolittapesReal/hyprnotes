#!/usr/bin/env python3
"""Isolated owner for tiny fixture children; never signals by bare PID."""
import base64
import json
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import process_guard

CHILD = r'''import os,sys,time
ready,stop=sys.argv[1:]
open(ready,"x").write("ready")
while not os.path.exists(stop): time.sleep(.01)
'''


def append_durable(path, row):
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    try:
        data = (json.dumps(row, sort_keys=True) + "\n").encode()
        os.write(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)


def private_dir(value):
    path = Path(value).absolute()
    st = os.lstat(path)
    if not Path(path).is_dir() or not __import__("stat").S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise ValueError("fixture directories must be private owned directories")
    if path.resolve(strict=True) != path:
        raise ValueError("fixture directory path must not traverse symlinks")
    return path


def append_error(evidence, event, name="", error=""):
    """Best-effort secret-free diagnostics, independent of injected ledger writer."""
    path = evidence / "errors.jsonl"
    try:
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        try:
            row = {"event": event, "name": name, "error_type": error}
            os.write(fd, (json.dumps(row, sort_keys=True) + "\n").encode())
            os.fsync(fd)
        finally:
            os.close(fd)
        os.chmod(path, 0o600)
        return True
    except OSError:
        return False


def run(request):
    evidence = private_dir(request["evidence"])
    work = private_dir(request["work"])
    if evidence == work or evidence.parent != work.parent:
        raise ValueError("evidence and work must be direct sibling directories")
    private_dir(evidence.parent)
    if request.get("scenario") != "assertion_failure":
        raise ValueError("unsupported fixture scenario")
    ledger = evidence / "ownership.jsonl"
    fd = os.open(ledger, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.fsync(fd)
    os.close(fd)

    children = []
    failure = None
    setup_succeeded = False
    identity_recorded = set()
    cleanup_failed = False
    waited = []
    marker_proven = set()
    wait_proven = set()

    def note_error(event, name, exc):
        nonlocal failure, cleanup_failed
        cleanup_failed = True
        failure = failure or (event + ":" + name)
        append_error(evidence, event, name, type(exc).__name__)

    try:
        for name in ("fake-compositor", "fake-app"):
            token = secrets.token_hex(32)
            ready = work / (name + ".ready")
            stop = work / (name + ".stop")
            env = dict(os.environ, HN_LIVE_OWNER_TOKEN=token)
            argv = [sys.executable, "-c", CHILD, str(ready), str(stop)]
            proc = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    close_fds=True)
            children.append((name, proc, ready, stop, token))
            append_durable(ledger, {"event": "spawned", "name": name, "pid": proc.pid,
                                    "run_id": evidence.name})
            deadline = time.monotonic() + 5
            while not ready.exists() and proc.poll() is None and time.monotonic() < deadline:
                time.sleep(.01)
            if not ready.is_file() or proc.poll() is not None:
                raise RuntimeError("fixture child readiness failed")
            identity = process_guard.snapshot(proc.pid, str(evidence), token)
            expected_argv = base64.b64encode((b"\0".join(os.fsencode(x) for x in argv) + b"\0")).decode()
            if identity["argv"] != expected_argv or identity["exe"] != os.path.realpath(sys.executable):
                raise RuntimeError("fixture child identity differs from creator request")
            append_durable(ledger, {"event": "created", "name": name, "run_id": evidence.name,
                                    "identity": identity})
            identity_recorded.add(name)

        # Deliberately simulate an assertion failure only after both identities
        # and actual Popen handles are retained by this isolated owner.
        setup_succeeded = True
        raise AssertionError("deliberate fixture assertion failure")
    except AssertionError as exc:
        if str(exc) != "deliberate fixture assertion failure":
            failure = "unexpected assertion failure"
    except Exception as exc:
        failure = failure or ("fixture setup:" + type(exc).__name__)
        append_error(evidence, "fixture_setup_failure", error=type(exc).__name__)
    finally:
        for name, proc, ready, stop, token in children:
            fd = None
            try:
                fd = os.open(stop, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                os.write(fd, b"stop")
                os.fsync(fd)
                os.close(fd)
                fd = None
                append_durable(ledger, {"event": "graceful_marker", "name": name,
                                        "pid": proc.pid, "run_id": evidence.name})
                marker_proven.add(name)
            except Exception as exc:
                # Existing marker may be from this exact helper's partial write;
                # still record failure and never treat it as durable proof.
                note_error("marker_or_ledger_failure", name, exc)
            finally:
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError as exc:
                        note_error("marker_close_failure", name, exc)

        for name, proc, ready, stop, token in children:
            code = None
            for attempt in range(2):
                try:
                    code = proc.wait(timeout=5)
                    break
                except subprocess.TimeoutExpired as exc:
                    note_error("wait_timeout", name, exc)
                except (OSError, ChildProcessError) as exc:
                    if attempt == 0:
                        append_error(evidence, "wait_retry", name, type(exc).__name__)
                    else:
                        note_error("wait_error", name, exc)
                except Exception as exc:
                    note_error("wait_error", name, exc)
                    break
            if code is None:
                continue
            if code != 0:
                note_error("nonzero_exit", name, RuntimeError("nonzero child status"))
                continue
            waited.append(name)
            wait_proven.add(name)
            try:
                append_durable(ledger, {"event": "wait_reaped", "name": name,
                                        "pid": proc.pid, "status": code, "run_id": evidence.name})
            except Exception as exc:
                note_error("wait_record_failure", name, exc)

        expected_names = {name for name, *_ in children}
        proof_complete = (len(children) == 2 and setup_succeeded
                          and identity_recorded == expected_names
                          and wait_proven == expected_names
                          and marker_proven == expected_names
                          and not cleanup_failed and failure is None)
        if proof_complete:
            try:
                append_durable(ledger, {"event": "proof_complete", "run_id": evidence.name,
                                        "waited_and_reaped": sorted(wait_proven), "remaining_children": 0})
            except Exception as exc:
                note_error("completion_record_failure", "", exc)
                proof_complete = False
        if proof_complete:
            try:
                for name, proc, ready, stop, token in children:
                    ready.unlink()
                    stop.unlink()
                work.rmdir()
            except Exception as exc:
                note_error("work_removal_failure", "", exc)

    all_reaped = len(waited) == len(children) and len(children) == 2

    if failure:
        raise RuntimeError(failure)
    if len(children) != 2 or len(waited) != 2 or not all_reaped or cleanup_failed:
        raise RuntimeError("fixture children not all positively waited/reaped")
    return {"scenario": request["scenario"], "simulated_assertion_failed": True,
            "children_requested": [name for name, *_ in children],
            "waited_and_reaped": waited, "remaining_children": 0}


def main():
    request = json.loads(sys.stdin.readline())
    result = run(request)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(type(exc).__name__ + ": " + str(exc), file=sys.stderr)
        raise SystemExit(1)
