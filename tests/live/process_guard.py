#!/usr/bin/env python3
"""Private PID ledger with pidfd-only cleanup for the live-test harness."""
import base64
import json
import os
import signal
import stat
import sys


def snapshot(pid, config, token):
    proc = f"/proc/{pid}"
    with open(proc + "/cmdline", "rb") as stream:
        raw = stream.read()
    args = raw.split(b"\0")[:-1]
    with open(proc + "/environ", "rb") as stream:
        environ = stream.read()
    if not args or environ.split(b"\0").count(b"HN_LIVE_OWNER_TOKEN=" + token.encode()) != 1:
        raise ValueError("launch token/argv absent")
    if os.path.basename(os.fsdecode(args[0])) == "Hyprland":
        expected = os.fsencode(os.path.realpath(config))
        if not any(args[i:i + 2] == [b"-c", expected] for i in range(len(args) - 1)):
            raise ValueError("Hyprland argv does not name exact nested config")
    with open(proc + "/stat") as stream:
        stat_fields = stream.read().rsplit(")", 1)[1].split()
    return {
        "pid": int(pid),
        "uid": os.stat(proc).st_uid,
        "start": stat_fields[19],
        "exe": os.readlink(proc + "/exe"),
        "argv": base64.b64encode(raw).decode("ascii"),
        "config": os.path.realpath(config),
        "token": token,
    }


def signal_record(record, config, sig=signal.SIGTERM):
    """Signal only exact recorded process through stable pidfd; no kill(2) fallback."""
    fd = None
    try:
        pid = int(record["pid"])
        fd = os.pidfd_open(pid)
        current = snapshot(pid, config, record["token"])
        if current != record:
            return False
        signal.pidfd_send_signal(fd, sig)
        return True
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError):
        return False
    finally:
        if fd is not None:
            os.close(fd)


def matches(record, config):
    fd = None
    try:
        fd = os.pidfd_open(int(record["pid"]))
        return snapshot(record["pid"], config, record["token"]) == record
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError):
        return False
    finally:
        if fd is not None:
            os.close(fd)


def read_ledger(path):
    st = os.lstat(path)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o600:
        raise ValueError("ledger must be owned mode-0600 regular file")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "r", encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def main(argv):
    op, ledger, config = argv[1:4]
    if op == "record":
        print(json.dumps(snapshot(int(argv[4]), config, argv[5]), sort_keys=True))
    elif op == "stop":
        rows = read_ledger(ledger)
        for row in reversed(rows):
            print("signaled" if signal_record(row, config) else "refused")
    elif op == "check":
        pid = int(argv[4])
        raise SystemExit(0 if any(row.get("pid") == pid and matches(row, config) for row in read_ledger(ledger)) else 1)
    else:
        raise SystemExit("usage: process_guard.py record|stop|check LEDGER CONFIG [PID TOKEN]")


if __name__ == "__main__":
    main(sys.argv)
