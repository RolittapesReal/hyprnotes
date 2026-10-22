#!/usr/bin/env python3
"""Durable, fail-closed lifecycle controller for the offline live harness."""
import fcntl
import hashlib
import json
import os
import secrets
import shutil
import signal
import stat
import sys
import time
import uuid
from pathlib import Path

import process_guard

VERSION = 1
PHASES = {"PREPARED", "PENDING", "ACTIVE", "FINALIZING", "CLEAN", "UNSAFE"}


def fail(message="unsafe lifecycle state"):
    raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def identity(path):
    s = os.lstat(path)
    if not stat.S_ISDIR(s.st_mode) or s.st_uid != os.getuid() or stat.S_IMODE(s.st_mode) != 0o700:
        fail("directory must be current-UID-owned mode-0700 real directory")
    return {"dev": s.st_dev, "ino": s.st_ino}


def parent_identity(path):
    s = os.lstat(path)
    private = s.st_uid == os.getuid() and bool(s.st_mode & stat.S_IWUSR) and not (s.st_mode & 0o022)
    sticky_temp = s.st_uid == 0 and bool(s.st_mode & stat.S_ISVTX) and bool(s.st_mode & stat.S_IWOTH)
    if not stat.S_ISDIR(s.st_mode) or not (private or sticky_temp): fail("unsafe scratch parent directory")
    return {"dev": s.st_dev, "ino": s.st_ino}


def no_symlink_components(path, allow_missing_leaf=False):
    p = Path(os.path.abspath(path))
    cur = Path(p.anchor)
    for index, part in enumerate(p.parts[1:], 1):
        cur /= part
        try: item = os.lstat(cur)
        except FileNotFoundError:
            if allow_missing_leaf and index == len(p.parts) - 1: return False
            fail("path component missing")
        if stat.S_ISLNK(item.st_mode) or not stat.S_ISDIR(item.st_mode): fail("path component is not a real directory")
        mode = stat.S_IMODE(item.st_mode)
        if mode & 0o022 and not (item.st_mode & stat.S_ISVTX and item.st_uid == 0):
            fail("unsafe writable path ancestor")
    return True


def within(a, b):
    return os.path.commonpath((os.path.realpath(a), os.path.realpath(b))) in (os.path.realpath(a), os.path.realpath(b))


def proc_start(pid):
    with open(f"/proc/{pid}/stat") as f: fields = f.read().rsplit(")", 1)[1].split()
    return fields[19]


def proc_uid(pid):
    return os.stat(f"/proc/{pid}").st_uid


def private_file(path):
    s = os.lstat(path)
    if not stat.S_ISREG(s.st_mode) or s.st_uid != os.getuid() or stat.S_IMODE(s.st_mode) != 0o600:
        fail("private file ownership/mode invalid")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    opened = os.fstat(fd)
    if (opened.st_dev, opened.st_ino) != (s.st_dev, s.st_ino) or not stat.S_ISREG(opened.st_mode) or opened.st_uid != os.getuid() or stat.S_IMODE(opened.st_mode) != 0o600:
        os.close(fd); fail("private file changed while opening")
    with os.fdopen(fd, "rb") as f: return f.read()


def atomic(path, data):
    parent = os.path.dirname(path)
    name = ".tmp-" + secrets.token_hex(12)
    fd = os.open(os.path.join(parent, name), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb", closefd=False) as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(os.path.join(parent, name), path)
        dfd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY); os.fsync(dfd); os.close(dfd)
    except BaseException:
        try: os.unlink(os.path.join(parent, name))
        except OSError: pass
        raise


def write_json(path, obj): atomic(path, (json.dumps(obj, sort_keys=True, separators=(",", ":")) + "\n").encode())


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result: fail("duplicate JSON key")
        result[key] = value
    return result


def load_json(raw):
    try: return json.loads(raw, object_pairs_hook=unique_pairs)
    except RuntimeError: raise
    except Exception: fail("malformed private JSON")


def hex_id(value):
    return isinstance(value, str) and len(value) == 32 and all(c in "0123456789abcdef" for c in value)


def hex_n(value, length):
    return isinstance(value, str) and len(value) == length and all(c in "0123456789abcdef" for c in value)


def digest_string(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def identity_object(value):
    return (isinstance(value, dict) and set(value) == {"dev", "ino"}
            and all(type(value[k]) is int and value[k] >= 0 for k in ("dev", "ino")))


def read_json(path):
    raw = private_file(path)
    obj = load_json(raw)
    if not isinstance(obj, dict): fail("malformed private JSON")
    return obj


def state_path(session): return os.path.join(session, "state.json")


def validate(session):
    session = os.path.abspath(session)
    no_symlink_components(session); identity(session)
    st = read_json(state_path(session))
    required = {"version", "invocation", "session", "owner_pid", "owner_uid", "owner_start", "evidence", "evidence_id", "scratch", "scratch_id", "parent", "parent_id", "stage", "stage_id", "config_sha256", "phase", "pending", "records", "staging", "exit_proofs", "setup"}
    if set(st) != required or type(st["version"]) is not int or st["version"] != VERSION or not isinstance(st["phase"], str) or st["phase"] not in PHASES: fail("invalid state schema")
    if not hex_id(st["invocation"]) or not isinstance(st["session"], str) or st["session"] != "session-" + st["invocation"] or st["session"] != os.path.basename(session): fail("session binding mismatch")
    if type(st["owner_pid"]) is not int or st["owner_pid"] <= 0 or type(st["owner_uid"]) is not int or st["owner_uid"] != os.getuid(): fail("invalid owner identity")
    if not isinstance(st["owner_start"], str) or not st["owner_start"].isdigit(): fail("invalid owner start identity")
    for key in ("evidence", "scratch", "parent", "stage"):
        if not isinstance(st[key], str) or not os.path.isabs(st[key]) or os.path.normpath(st[key]) != st[key]: fail("invalid pinned path")
    if os.path.dirname(session) != st["evidence"] or os.path.dirname(st["scratch"]) != st["parent"] or os.path.dirname(st["stage"]) != st["parent"]: fail("pinned path relationship mismatch")
    if within(st["evidence"], st["scratch"]) or within(st["stage"], st["evidence"]) or within(st["evidence"], st["stage"]): fail("pinned paths overlap")
    if st["scratch_id"]["dev"] != st["parent_id"]["dev"]: fail("scratch/parent filesystem mismatch")
    if not all(identity_object(st[k]) for k in ("evidence_id", "scratch_id", "parent_id")): fail("invalid pinned identity schema")
    if st["stage_id"] is not None and not identity_object(st["stage_id"]): fail("invalid staging root identity")
    if st["config_sha256"] is not None and not digest_string(st["config_sha256"]): fail("invalid config digest")
    setup = st["setup"]
    if not isinstance(setup, dict) or set(setup) != {"status", "stage_name", "init_record"}: fail("invalid setup schema")
    if (setup["status"] not in ("intent", "ready") or not isinstance(setup["stage_name"], str)
            or setup["stage_name"] != os.path.basename(st["stage"])
            or not setup["stage_name"].startswith(".hn-stage-")
            or not hex_n(setup["stage_name"][len(".hn-stage-"):], 20)): fail("invalid setup binding")
    if setup["init_record"] != "init-" + st["invocation"] + ".json": fail("invalid setup intent reference")
    if (setup["status"] == "ready") != (st["phase"] != "UNSAFE" and st["stage_id"] is not None): fail("setup phase contradiction")
    if st["phase"] != "UNSAFE" and setup["status"] != "ready": fail("incomplete initialization")
    if not isinstance(st["records"], list) or not isinstance(st["exit_proofs"], list): fail("invalid record/proof arrays")
    seen = set()
    for rec in st["records"]:
        if not isinstance(rec, dict) or set(rec) != {"name", "sha256", "kind", "pid"}: fail("invalid record descriptor schema")
        record_id = rec["name"][7:-5] if isinstance(rec["name"], str) and rec["name"].startswith("record-") and rec["name"].endswith(".json") else None
        if not isinstance(rec["name"], str) or rec["name"] in seen or not hex_id(record_id): fail("invalid/duplicate record name")
        if not digest_string(rec["sha256"]): fail("invalid record digest")
        if rec["kind"] not in ("app", "compositor") or type(rec["pid"]) is not int or rec["pid"] <= 0: fail("invalid record identity")
        seen.add(rec["name"])
    if st["phase"] == "PREPARED" and (st["pending"] is not None or st["records"]): fail("PREPARED state contains launch data")
    if (st["phase"] == "PENDING") != (st["pending"] is not None): fail("pending phase contradiction")
    if st["pending"] is not None:
        p = st["pending"]
        if (not isinstance(p, dict) or set(p) != {"id", "kind", "executable", "argv"}
                or not hex_id(p["id"]) or p["kind"] not in ("app", "compositor")
                or not isinstance(p["executable"], str) or not os.path.isabs(p["executable"])
                or not isinstance(p["argv"], list) or not p["argv"] or any(not isinstance(x, str) for x in p["argv"])
                or os.path.realpath(p["argv"][0]) != p["executable"]): fail("invalid pending intent schema")
    if st["staging"] is not None:
        tx = st["staging"]
        keys = {"source", "name", "source_id", "stage_id", "status", "deletion"}
        if not isinstance(tx, dict) or set(tx) != keys: fail("invalid staging journal schema")
        if tx["source"] != os.path.basename(st["scratch"]) or tx["name"] != "tree-" + st["session"]: fail("staging journal path mismatch")
        if tx["source_id"] != st["scratch_id"] or tx["stage_id"] != st["stage_id"]: fail("staging journal identity mismatch")
        if tx["status"] not in ("intent", "renamed") or tx["deletion"] not in ("not_started", "deleting", "removed"): fail("invalid staging journal phase")
    for proof in st["exit_proofs"]:
        if (not isinstance(proof, dict) or set(proof) != {"record_sha256", "exited"}
                or not digest_string(proof["record_sha256"]) or proof["exited"] is not True): fail("invalid exit proof")
    if st["phase"] == "FINALIZING" and st["exit_proofs"] != [{"record_sha256": r["sha256"], "exited": True} for r in st["records"]]: fail("finalizing state lacks exact exit proof")
    no_symlink_components(st["evidence"]); no_symlink_components(st["parent"])
    no_symlink_components(st["stage"], allow_missing_leaf=st["setup"]["status"] == "intent")
    if identity(st["evidence"]) != st["evidence_id"] or parent_identity(st["parent"]) != st["parent_id"]: fail("pinned root identity changed")
    if st["setup"]["status"] == "ready" and identity(st["stage"]) != st["stage_id"]: fail("pinned stage identity changed")
    scratch_present = no_symlink_components(st["scratch"], allow_missing_leaf=st["phase"] in ("FINALIZING", "CLEAN"))
    if not scratch_present and st["phase"] not in ("FINALIZING", "CLEAN"): fail("scratch path unexpectedly absent")
    if st["phase"] not in ("FINALIZING", "CLEAN", "UNSAFE") and identity(st["scratch"]) != st["scratch_id"]: fail("pinned scratch identity changed")
    init_name = "init-" + st["invocation"] + ".json"
    intent = read_json(os.path.join(st["evidence"], init_name))
    expected_intent = {"version": VERSION, "invocation": st["invocation"], "session": st["session"],
                       "owner_pid": st["owner_pid"], "owner_uid": st["owner_uid"], "owner_start": st["owner_start"],
                       "evidence": st["evidence"], "evidence_id": st["evidence_id"], "scratch": st["scratch"],
                       "scratch_id": st["scratch_id"], "parent": st["parent"], "parent_id": st["parent_id"],
                       "stage": st["stage"], "stage_name": st["setup"]["stage_name"]}
    if set(intent) != set(expected_intent) or any(intent.get(k) != v for k, v in expected_intent.items()): fail("initialization intent binding mismatch")
    if st["config_sha256"] is None:
        if os.path.lexists(os.path.join(session, "hyprland.lua")): fail("unexpected config without committed digest")
    elif digest(private_file(os.path.join(session, "hyprland.lua"))) != st["config_sha256"]: fail("durable config digest mismatch")
    token = private_file(os.path.join(session, "owner-token"))
    if not token: fail("empty owner token")
    return st


class Locked:
    def __init__(self, session):
        self.session = os.path.abspath(session)
    def __enter__(self):
        no_symlink_components(self.session)
        identity(self.session)
        lockpath = os.path.join(self.session, "lock")
        lockstat = os.lstat(lockpath)
        if not stat.S_ISREG(lockstat.st_mode) or lockstat.st_uid != os.getuid() or stat.S_IMODE(lockstat.st_mode) != 0o600: fail("invalid lock file")
        self.fd = os.open(lockpath, os.O_RDWR | os.O_NOFOLLOW)
        opened = os.fstat(self.fd)
        if (opened.st_dev, opened.st_ino) != (lockstat.st_dev, lockstat.st_ino): os.close(self.fd); fail("lock file changed while opening")
        try: fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError: os.close(self.fd); fail("session busy")
        self.state = validate(self.session)
        return self.state
    def __exit__(self, *exc): fcntl.flock(self.fd, fcntl.LOCK_UN); os.close(self.fd)


def save(session, state): write_json(state_path(session), state)


def exclusive_json(path, obj):
    data = (json.dumps(obj, sort_keys=True, separators=(",", ":")) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
    dfd = os.open(os.path.dirname(path), os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(dfd)
    finally: os.close(dfd)


def init(evidence, scratch, owner):
    evidence, scratch = os.path.abspath(evidence), os.path.abspath(scratch)
    no_symlink_components(evidence); no_symlink_components(scratch)
    evidence_id = identity(evidence); scratch_id = identity(scratch)
    if within(evidence, scratch): fail("evidence and scratch overlap")
    parent = os.path.dirname(scratch); parent_id = parent_identity(parent)
    if scratch_id["dev"] != parent_id["dev"]: fail("scratch root cannot be staged on parent filesystem")
    owner = int(owner)
    if owner <= 0: fail("invalid owner PID")
    uid = proc_uid(owner); start = proc_start(owner)
    if uid != os.getuid(): fail("invocation owner must belong to current UID")
    invocation = uuid.uuid4().hex
    sid = "session-" + invocation
    session = os.path.join(evidence, sid)
    stage_name = ".hn-stage-" + secrets.token_hex(10)
    stage = os.path.join(parent, stage_name)
    if within(stage, evidence) or within(evidence, stage):
        fail("evidence and staging overlap")
    no_symlink_components(stage, allow_missing_leaf=True)
    intent_name = "init-" + invocation + ".json"
    intent = {"version": VERSION, "invocation": invocation, "session": sid, "owner_pid": owner,
              "owner_uid": uid, "owner_start": start, "evidence": evidence, "evidence_id": evidence_id,
              "scratch": scratch, "scratch_id": scratch_id, "parent": parent, "parent_id": parent_id,
              "stage": stage, "stage_name": stage_name}
    # Durable setup authority precedes session and staging side effects.
    exclusive_json(os.path.join(evidence, intent_name), intent)
    os.mkdir(session, 0o700)
    identity(session)
    token = secrets.token_urlsafe(32)
    fd = os.open(os.path.join(session, "lock"), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600); os.close(fd)
    fd = os.open(os.path.join(session, "owner-token"), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w") as f: f.write(token); f.flush(); os.fsync(f.fileno())
    state = {"version": VERSION, "invocation": invocation, "session": sid, "owner_pid": owner,
             "owner_uid": uid, "owner_start": start, "evidence": evidence, "evidence_id": evidence_id,
             "scratch": scratch, "scratch_id": scratch_id, "parent": parent, "parent_id": parent_id,
             "stage": stage, "stage_id": None, "config_sha256": None, "phase": "UNSAFE",
             "pending": None, "records": [], "staging": None, "exit_proofs": [],
             "setup": {"status": "intent", "stage_name": stage_name, "init_record": intent_name}}
    save(session, state)
    os.mkdir(stage, 0o700)
    stage_id = identity(stage)
    if stage_id["dev"] != scratch_id["dev"]: fail("staging directory is on a different filesystem")
    state["stage_id"] = stage_id
    state["setup"]["status"] = "ready"
    state["phase"] = "PREPARED"
    save(session, state)
    print(session)


def require_owner(st):
    try:
        if proc_uid(st["owner_pid"]) != st["owner_uid"] or proc_start(st["owner_pid"]) != st["owner_start"]: fail("invocation owner changed")
    except (OSError, IndexError): fail("invocation owner unavailable")


def config(session, data):
    with Locked(session) as st:
        if st["phase"] != "PREPARED" or st["pending"] or st["records"]: fail("config already in use")
        atomic(os.path.join(session, "hyprland.lua"), data)
        st["config_sha256"] = digest(data); save(session, st)


def reconfig(session, data):
    """Replace the durable compositor config of an ACTIVE session (monitor/scale checks).

    The digest moves with the file under the lock, so validate() invariants hold. Refused while
    a launch intent is pending or no compositor is recorded.
    """
    with Locked(session) as st:
        require_owner(st)
        if st["phase"] != "ACTIVE" or st["pending"] or not any(r["kind"] == "compositor" for r in st["records"]):
            fail("reconfig requires an active session with a recorded compositor")
        if not data or len(data) > 65536: fail("invalid replacement config")
        atomic(os.path.join(session, "hyprland.lua"), data)
        st["config_sha256"] = digest(data); save(session, st)


def intent(session, kind, executable, data):
    with Locked(session) as st:
        require_owner(st)
        if kind not in ("app", "compositor"): fail("invalid launch kind")
        if st["phase"] not in ("PREPARED", "ACTIVE") or st["pending"]: fail("launch not permitted")
        if not st["config_sha256"]: fail("config missing")
        if digest(private_file(os.path.join(session, "hyprland.lua"))) != st["config_sha256"]: fail("durable config digest changed")
        request = load_json(data)
        if not isinstance(request, dict) or set(request) != {"argv", "config_sha256"} or request["config_sha256"] != st["config_sha256"]: fail("invalid intent schema/config")
        if not isinstance(request["argv"], list) or not request["argv"] or any(not isinstance(x, str) for x in request["argv"]): fail("invalid argv")
        exe = os.path.realpath(executable)
        if os.path.realpath(request["argv"][0]) != exe: fail("executable mismatch")
        if kind == "compositor":
            # A second compositor is allowed only after every earlier one has proven exit.
            for d in st["records"]:
                if d["kind"] == "compositor" and not positive_exit(record_row(session, d)): fail("earlier compositor still live")
        intent_id = uuid.uuid4().hex
        st["pending"] = {"id": intent_id, "kind": kind, "executable": exe, "argv": request["argv"]}
        st["phase"] = "PENDING"; save(session, st); print(intent_id)


def record(session, kind, pid):
    with Locked(session) as st:
        require_owner(st)
        pending = st["pending"]
        if st["phase"] != "PENDING" or not pending or pending["kind"] != kind: fail("no matching pending intent")
        cfg = os.path.join(session, "hyprland.lua"); token = private_file(os.path.join(session, "owner-token")).decode()
        if digest(private_file(cfg)) != st["config_sha256"]: fail("durable config digest changed")
        row = process_guard.snapshot(int(pid), cfg, token)
        if row["exe"] != pending["executable"] or row_argv(row) != pending["argv"]:
            fail("snapshot does not match declared launch: " + repr((row["exe"], pending["executable"], row_argv(row), pending["argv"])))
        if kind == "compositor" and ("-c", cfg) not in list(zip(row_argv(row), row_argv(row)[1:])): fail("compositor config mismatch")
        payload = (json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode()
        name = "record-" + pending["id"] + ".json"; path = os.path.join(session, name)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f: f.write(payload); f.flush(); os.fsync(f.fileno())
        dfd = os.open(session, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(dfd)
        finally: os.close(dfd)
        st["records"].append({"name": name, "sha256": digest(payload), "kind": kind, "pid": int(pid)})
        st["pending"] = None; st["phase"] = "ACTIVE"; save(session, st)


def oneshot_exited(session, pid):
    """Resolve a pending app intent for a short-lived client the caller has already reaped.

    Long-lived apps (--background) must be recorded instead. The PID must be absent from /proc;
    an audit file keeps the intent and PID durable. No signalling is involved.
    """
    with Locked(session) as st:
        require_owner(st)
        pending = st["pending"]
        if st["phase"] != "PENDING" or not pending or pending["kind"] != "app": fail("no matching pending app intent")
        if len(pending["argv"]) != 2 or pending["argv"][1] not in ("--new-note", "--show-organizer"):
            fail("only a known one-shot client action may be abandoned")
        pid = int(pid)
        if pid <= 0 or os.path.exists("/proc/%d" % pid): fail("oneshot process still present")
        exclusive_json(os.path.join(session, "oneshot-" + pending["id"] + ".json"),
                       {"id": pending["id"], "argv": pending["argv"], "pid": pid, "exited": True})
        st["pending"] = None; st["phase"] = "ACTIVE"; save(session, st)


def row_argv(row):
    import base64
    return [os.fsdecode(x) for x in base64.b64decode(row["argv"]).split(b"\0")[:-1]]


def record_row(session, descriptor):
    raw = private_file(os.path.join(session, descriptor["name"]))
    if digest(raw) != descriptor["sha256"]: fail("record digest mismatch")
    row = load_json(raw)
    if not isinstance(row, dict) or set(row) != {"pid", "uid", "start", "exe", "argv", "config", "token"}: fail("guard record schema mismatch")
    if type(row["pid"]) is not int or row["pid"] != descriptor["pid"] or type(row["uid"]) is not int or row["uid"] != os.getuid(): fail("record identity mismatch")
    if not isinstance(row["start"], str) or not row["start"].isdigit() or not isinstance(row["exe"], str) or not os.path.isabs(row["exe"]): fail("malformed guard record identity")
    if row["config"] != os.path.join(session, "hyprland.lua") or row["token"] != private_file(os.path.join(session, "owner-token")).decode(): fail("guard record binding mismatch")
    try:
        import base64
        args = base64.b64decode(row["argv"], validate=True)
    except Exception: fail("malformed guard record argv")
    if not args.endswith(b"\0") or not args.strip(b"\0"): fail("malformed guard record argv")
    return row


def positive_exit(row):
    try:
        uid = proc_uid(row["pid"]); start = proc_start(row["pid"])
    except FileNotFoundError: return True
    except (PermissionError, OSError, IndexError): return False
    if uid != row["uid"] or start != row["start"]: return False
    try:
        with open(f"/proc/{row['pid']}/stat") as f: fields = f.read().rsplit(")", 1)[1].split()
        return fields[0] == "Z"
    except OSError: return False


def inode_matches(st, expected):
    return (st.st_dev, st.st_ino) == (expected.st_dev, expected.st_ino)


def remove_tree_fd(fd, roots_valid):
    for name in os.listdir(fd):
        roots_valid()
        try: st = os.stat(name, dir_fd=fd, follow_symlinks=False)
        except FileNotFoundError: fail("staging entry changed")
        if stat.S_ISDIR(st.st_mode):
            child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            try:
                opened = os.fstat(child)
                if not inode_matches(opened, st): fail("directory swapped between stat and open")
                current = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not inode_matches(current, st): fail("directory name no longer binds opened inode")
                remove_tree_fd(child, roots_valid)
                roots_valid()
                if not inode_matches(os.fstat(child), st): fail("opened directory identity changed")
                current = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not inode_matches(current, st): fail("directory swapped before rmdir")
                roots_valid()
                current = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not inode_matches(current, st): fail("directory swapped at rmdir boundary")
                os.rmdir(name, dir_fd=fd)
            finally: os.close(child)
        else:
            if not hasattr(os, "O_PATH"): fail("safe descriptor deletion unavailable")
            entry = os.open(name, os.O_PATH | os.O_NOFOLLOW, dir_fd=fd)
            try:
                if not inode_matches(os.fstat(entry), st): fail("entry swapped between stat and open")
                current = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not inode_matches(current, st): fail("entry name no longer binds opened inode")
                roots_valid()
                current = os.stat(name, dir_fd=fd, follow_symlinks=False)
                if not inode_matches(current, st) or not inode_matches(os.fstat(entry), st): fail("entry swapped before unlink")
                roots_valid()
                os.unlink(name, dir_fd=fd)
            finally: os.close(entry)


def clean_scratch(session, st):
    parentfd = os.open(st["parent"], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    stagefd = os.open(st["stage"], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        def roots_valid():
            for fd, path, pinned in ((parentfd, st["parent"], st["parent_id"]),
                                     (stagefd, st["stage"], st["stage_id"])):
                opened = os.fstat(fd)
                try: named = os.stat(path, follow_symlinks=False)
                except OSError: fail("pinned directory path unavailable")
                if (opened.st_dev, opened.st_ino) != (pinned["dev"], pinned["ino"]) or not inode_matches(named, opened): fail("pinned directory identity changed")
                no_symlink_components(path)
                current_id = parent_identity(path) if path == st["parent"] else identity(path)
                if current_id != pinned: fail("pinned directory mode changed")
        roots_valid()
        source_name = os.path.basename(st["scratch"])
        def expected_scratch(stbuf):
            return (stbuf.st_dev, stbuf.st_ino) == (st["scratch_id"]["dev"], st["scratch_id"]["ino"]) and stat.S_ISDIR(stbuf.st_mode) and stbuf.st_uid == os.getuid() and stat.S_IMODE(stbuf.st_mode) == 0o700
        if st["staging"] is None:
            roots_valid()
            src = os.stat(source_name, dir_fd=parentfd, follow_symlinks=False)
            if not expected_scratch(src): fail("scratch identity changed")
            name = "tree-" + st["session"]
            if os.listdir(stagefd): fail("staging directory not empty")
            st["staging"] = {"source": source_name, "name": name, "source_id": dict(st["scratch_id"]),
                             "stage_id": dict(st["stage_id"]), "status": "intent", "deletion": "not_started"}
            save(session, st)
        tx = st["staging"]
        if tx["source"] != source_name or tx["source_id"] != st["scratch_id"] or tx["stage_id"] != st["stage_id"]: fail("staging journal binding mismatch")
        staged = tx["name"]
        if tx["status"] == "intent":
            try: src = os.stat(source_name, dir_fd=parentfd, follow_symlinks=False)
            except FileNotFoundError: src = None
            try: moved = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            except FileNotFoundError: moved = None
            if src is not None and moved is None:
                roots_valid()
                if os.listdir(stagefd): fail("unexpected staging entry")
                if not expected_scratch(src): fail("source changed before staging")
                os.rename(source_name, staged, src_dir_fd=parentfd, dst_dir_fd=stagefd)
                os.fsync(parentfd); os.fsync(stagefd)
                roots_valid()
                moved = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            elif src is not None or moved is None:
                fail("ambiguous source/staging location")
            elif os.listdir(stagefd) != [staged]:
                fail("unexpected staging entry")
            if not expected_scratch(moved): fail("staged tree identity mismatch")
            tx["status"] = "renamed"; save(session, st)
        if tx["status"] != "renamed" or tx["deletion"] not in ("not_started", "deleting", "removed"): fail("invalid staging journal phase")
        if tx["deletion"] == "removed":
            if os.listdir(stagefd): fail("unexpected staging entry after deletion")
            try: os.stat(source_name, dir_fd=parentfd, follow_symlinks=False)
            except FileNotFoundError: pass
            else: fail("unexpected source reappeared after deletion")
            try: os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            except FileNotFoundError: return
            fail("removed staging tree reappeared")
        moved = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
        if not expected_scratch(moved): fail("staged tree identity changed")
        if tx["deletion"] == "not_started":
            tx["deletion"] = "deleting"; save(session, st)
        try: os.stat(source_name, dir_fd=parentfd, follow_symlinks=False)
        except FileNotFoundError: pass
        else: fail("unexpected source reappeared after staging rename")
        if os.listdir(stagefd) != [staged]: fail("unexpected staging entry")
        roots_valid()
        treefd = os.open(staged, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=stagefd)
        try:
            opened_tree = os.fstat(treefd)
            if not expected_scratch(opened_tree): fail("staged root swapped between stat and open")
            named_tree = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            if not expected_scratch(named_tree): fail("staged root name changed")
            remove_tree_fd(treefd, roots_valid)
            roots_valid()
            if not expected_scratch(os.fstat(treefd)): fail("staged root descriptor identity changed")
            named_tree = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            if not expected_scratch(named_tree): fail("staged root swapped before rmdir")
            roots_valid()
            named_tree = os.stat(staged, dir_fd=stagefd, follow_symlinks=False)
            if not expected_scratch(named_tree): fail("staged root swapped at rmdir boundary")
            os.rmdir(staged, dir_fd=stagefd)
        finally: os.close(treefd)
        os.fsync(stagefd); os.fsync(parentfd)
        tx["deletion"] = "removed"; save(session, st)
    finally: os.close(stagefd); os.close(parentfd)


def receipt_data(st):
    return {"version": VERSION, "session": st["session"], "invocation": st["invocation"],
            "records": st["records"], "scratch_id": st["scratch_id"],
            "exits": st["exit_proofs"], "staging": st["staging"],
            "scratch_removed": True, "state": "CLEAN"}


def verify_receipt(session, st, allow_finalizing=False):
    receipt = read_json(os.path.join(session, "receipt.json"))
    valid_phase = st["phase"] == "CLEAN" or (allow_finalizing and st["phase"] == "FINALIZING")
    if not valid_phase or st["staging"] is None or st["staging"]["deletion"] != "removed" or receipt != receipt_data(st): fail("receipt binding invalid")
    return receipt


def finish(session):
    with Locked(session) as st:
        if st["phase"] == "CLEAN": verify_receipt(session, st); return
        if st["phase"] == "FINALIZING":
            expected_proofs = [{"record_sha256": d["sha256"], "exited": True} for d in st["records"]]
            if st["exit_proofs"] != expected_proofs: fail("missing or contradictory durable exit proof")
            rows = [record_row(session, d) for d in st["records"]]
            for row in rows:
                if not positive_exit(row): fail("previously exited identity no longer proves exit")
            if st["staging"] is None or st["staging"]["deletion"] != "removed": clean_scratch(session, st)
            receipt_path = os.path.join(session, "receipt.json")
            if os.path.exists(receipt_path): verify_receipt(session, st, allow_finalizing=True)
            else:
                exclusive_json(receipt_path, receipt_data(st))
            st["phase"] = "CLEAN"; save(session, st)
            return
        if st["pending"] or st["phase"] not in ("PREPARED", "ACTIVE"): fail("unresolved launch intent")
        rows = [record_row(session, d) for d in st["records"]]
        if not rows and st["phase"] != "PREPARED": fail("no-launch proof invalid")
        proofs = []
        for row, descriptor in zip(reversed(rows), reversed(st["records"])):
            if not positive_exit(row):
                if not process_guard.signal_record(row, os.path.join(session, "hyprland.lua"), signal.SIGTERM): fail("guarded stop refused")
                deadline = time.monotonic() + 15.0  # nested compositor with clients can take seconds
                while time.monotonic() < deadline and not positive_exit(row): time.sleep(0.1)
                if not positive_exit(row): fail("process did not exit")
            proofs.append({"record_sha256": descriptor["sha256"], "exited": True})
        st["exit_proofs"] = list(reversed(proofs)); st["phase"] = "FINALIZING"; save(session, st)
        clean_scratch(session, st)
        exclusive_json(os.path.join(session, "receipt.json"), receipt_data(st))
        st["phase"] = "CLEAN"; save(session, st)


def main(argv):
    if len(argv) < 2: fail("usage: lifecycle.py init|config|intent|record|finish|verify-clean ...")
    op = argv[1]
    if op == "init" and len(argv) == 5: init(argv[2], argv[3], argv[4])
    elif op == "config" and len(argv) == 3: config(argv[2], sys.stdin.buffer.read())
    elif op == "reconfig" and len(argv) == 3: reconfig(argv[2], sys.stdin.buffer.read())
    elif op == "intent" and len(argv) == 5: intent(argv[2], argv[3], argv[4], sys.stdin.read())
    elif op == "record" and len(argv) == 5: record(argv[2], argv[3], argv[4])
    elif op == "oneshot-exited" and len(argv) == 4: oneshot_exited(argv[2], argv[3])
    elif op == "finish" and len(argv) == 3: finish(argv[2])
    elif op == "verify-clean" and len(argv) == 3:
        with Locked(argv[2]) as st: verify_receipt(argv[2], st)
    elif op == "status" and len(argv) == 3:
        with Locked(argv[2]) as st: print(st["phase"])
    else: fail("invalid lifecycle command")


if __name__ == "__main__":
    try: main(sys.argv)
    except BaseException as exc:
        print(str(exc) if isinstance(exc, RuntimeError) else "lifecycle operation failed", file=sys.stderr)
        raise SystemExit(2)
