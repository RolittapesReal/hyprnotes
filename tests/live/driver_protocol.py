#!/usr/bin/env python3
import json
import os
import selectors
import signal
import subprocess
import sys
import tempfile
import time
import time
from pathlib import Path


class Driver:
    def __init__(self, command, env, timeout=8):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     env=env, bufsize=0)
        self.timeout = timeout
        self.next_id = 1
        self.pending = bytearray()
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)

    def read_reply(self, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            end = self.pending.find(b"\n")
            if end >= 0:
                line = bytes(self.pending[:end])
                del self.pending[:end + 1]
                return json.loads(line)
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(remaining):
                raise TimeoutError("Qt driver did not answer before deadline")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f"Qt driver exited early ({self.proc.poll()}): {self.proc.stderr.read().decode(errors='replace')}")
            self.pending.extend(chunk)

    def request(self, op, **fields):
        request_id = self.next_id
        self.next_id += 1
        message = {"id": request_id, "op": op, **fields}
        self.proc.stdin.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        self.proc.stdin.flush()
        while True:
            reply = self.read_reply()
            if reply.get("id") != request_id:
                raise AssertionError(f"reply id mismatch: expected {request_id}, got {reply}")
            return reply

    def raw(self, data):
        self.proc.stdin.write(data + b"\n")
        self.proc.stdin.flush()
        return self.read_reply()

    def stop(self, graceful=True):
        if self.proc.poll() is None:
            if graceful:
                try:
                    self.request("quit")
                except (TimeoutError, BrokenPipeError, RuntimeError):
                    pass
            if self.proc.poll() is None:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=4)
        self.selector.close()


def scratch_env(root, base=None):
    env = dict(os.environ if base is None else base)
    for name, folder in {
        "HOME": "home", "XDG_CONFIG_HOME": "xdg-config", "XDG_DATA_HOME": "xdg-data",
        "XDG_STATE_HOME": "xdg-state", "XDG_CACHE_HOME": "xdg-cache",
        "HN_CONFIG_DIR": "hn-config", "HN_DATA_DIR": "hn-data", "HN_STATE_DIR": "hn-state",
        "HN_CACHE_DIR": "hn-cache", "HN_NOTES_DIR": "notes",
    }.items():
        path = Path(root, folder)
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    return env


def selftest(driver_path):
    with tempfile.TemporaryDirectory(prefix="hyprnotes-live-driver-") as scratch:
        env = scratch_env(scratch)
        env.update(QT_QPA_PLATFORM="offscreen", HN_LIVE_NO_HYPRLAND="1", HN_LIVE_NO_TRAY="1")
        driver = Driver([driver_path], env)
        try:
            malformed = driver.raw(b"not-json")
            assert malformed.get("ok") is False and malformed.get("error") == "invalid JSON request", malformed
            unknown = driver.request("not-an-operation")
            assert unknown.get("ok") is False and unknown.get("error") == "unknown operation", unknown
            created = driver.request("new")
            assert created.get("ok") is True and created.get("rel"), created
            rel = created["rel"]
            assert driver.request("workspace-mode", rel=rel, mode="all").get("ok") is True
            assert driver.request("workspace-mode", rel=rel, mode="this").get("ok") is True
            assert driver.request("peer").get("title") == "task6-peer"
            assert driver.request("close-peer").get("ok") is True
            assert driver.request("organizer").get("ok") is True
            assert driver.request("settings").get("ok") is True
            assert driver.request("consent").get("ok") is True
            for surface in ("organizer", "sticky", "settings", "consent"):
                geometry = driver.request("geometry", surface=surface, rel=rel)
                assert geometry.get("ok") and geometry.get("width", 0) > 0 and geometry.get("height", 0) > 0, geometry
                if surface == "consent":
                    assert geometry["height"] <= int(geometry["screenHeight"] * 0.9) + 2, geometry
                image = Path(scratch, surface + ".png")
                assert driver.request("screenshot", surface=surface, rel=rel, path=str(image)).get("ok") is True
                assert image.exists() and image.stat().st_size > 100
            assert driver.request("dismiss-dialog", surface="consent").get("ok") is True
            assert driver.request("dismiss-dialog", surface="settings").get("ok") is True
            assert driver.request("theme", scheme="dark").get("ok") is True
            assert driver.request("theme", scheme="light").get("ok") is True
            assert driver.request("storm-start", count=25).get("ok") is True
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                progress = driver.request("storm-progress")
                if progress.get("waitingForRelease"):
                    assert driver.request("storm-release").get("ok") is True
                elif progress.get("active"):
                    assert driver.request("storm-next").get("ok") is True
                if not progress.get("active") and progress.get("closed") == 25:
                    break
                time.sleep(0.01)
            progress = driver.request("storm-progress")
            assert progress.get("created") == 25 and progress.get("closed") == 25 and not progress.get("active"), progress
            edited = driver.request("edit", rel=rel, text="driver-smoke")
            assert edited.get("ok") and edited.get("text", "").endswith("driver-smoke"), edited
            saved = driver.request("save-note", rel=rel)
            assert saved.get("ok") and saved.get("state") == "Saved", saved
            assert Path(env["HN_NOTES_DIR"], rel).read_bytes() == b"driver-smoke\n"
            assert driver.request("flush").get("ok") is True
            os.kill(driver.proc.pid, signal.SIGTERM)
            assert driver.proc.wait(timeout=10) == 0, "driver did not exit cleanly after SIGTERM"
            driver.selector.close()

            driver = Driver([driver_path], env)
            restored = driver.request("state")
            assert restored.get("sessions") == [rel] and [w["rel"] for w in restored.get("windows", [])] == [rel], restored
            assert driver.request("flush").get("ok") is True
            os.kill(driver.proc.pid, signal.SIGKILL)
            assert driver.proc.wait(timeout=5) == -signal.SIGKILL, "driver did not terminate from SIGKILL"
            driver.selector.close()

            driver = Driver([driver_path], env)
            restored = driver.request("state")
            assert restored.get("sessions") == [rel] and [w["rel"] for w in restored.get("windows", [])] == [rel], restored
            assert Path(env["HN_NOTES_DIR"], rel).read_bytes() == b"driver-smoke\n"
            assert driver.request("close", rel=rel).get("ok") is True
            closed = driver.request("state")
            assert closed.get("sessions") == [] and closed.get("windows") == [], closed
            assert driver.request("quit").get("quit") is True
            driver.proc.wait(timeout=4)
        finally:
            driver.stop(graceful=False)

    sleeper = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(2)"], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    timeout_driver = Driver.__new__(Driver)
    timeout_driver.proc = sleeper
    timeout_driver.timeout = 0.1
    timeout_driver.next_id = 1
    timeout_driver.pending = bytearray()
    timeout_driver.selector = selectors.DefaultSelector()
    timeout_driver.selector.register(sleeper.stdout, selectors.EVENT_READ)
    try:
        try:
            timeout_driver.request("expected-timeout")
        except TimeoutError:
            pass
        else:
            raise AssertionError("missing reply did not time out")
    finally:
        sleeper.kill()
        sleeper.wait(timeout=4)
        timeout_driver.selector.close()
    print("PASS driver protocol: malformed JSON, unknown operation, create/edit/save, SIGTERM/SIGKILL restore, close, quit, timeout")


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--self-test":
        raise SystemExit("usage: driver_protocol.py --self-test DRIVER")
    selftest(sys.argv[2])
