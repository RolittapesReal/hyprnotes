#!/usr/bin/env python3
"""Offline process identity tests. Every process spawned here is a test-owned child."""
import json
import ctypes
import os
from pathlib import Path
import signal
import shlex
import subprocess
import sys
import tempfile
import time
import shutil
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import process_guard
import placement_gate
import command_builders


class ProcessGuardTest(unittest.TestCase):
    def setUp(self):
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)
        self.tmp = tempfile.TemporaryDirectory(prefix="hn-pidguard-")
        self.root = Path(self.tmp.name)
        self.config = str(self.root / "hyprland.lua")
        Path(self.config).write_text("-- fixture only\n")
        self.ledger = self.root / "pids"
        self.ledger.touch(mode=0o600)
        self.ledger.chmod(0o600)
        self.children = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=3)
        self.tmp.cleanup()

    def child(self, token="fixture-owned-token", args=(), config=None):
        marker = self.root / f"signal-{len(self.children)}"
        ready = self.root / f"ready-{len(self.children)}"
        code = "import signal,time,pathlib,sys; signal.signal(signal.SIGTERM,lambda *_:(pathlib.Path(sys.argv[1]).write_text('TERM'),sys.exit(0))); pathlib.Path(sys.argv[2]).write_text('READY'); time.sleep(60)"
        env = dict(os.environ, HN_LIVE_OWNER_TOKEN=token)
        proc = subprocess.Popen([sys.executable, "-c", code, str(marker), str(ready), *args], env=env)
        self.children.append(proc)
        for _ in range(200):
            try:
                if not ready.exists():
                    time.sleep(.005)
                    continue
                rec = process_guard.snapshot(proc.pid, config or self.config, token)
                return proc, marker, rec
            except (OSError, ValueError):
                time.sleep(.005)
        self.fail("controlled child failed to publish proc identity")

    def write(self, *rows):
        self.ledger.write_text("".join(json.dumps(x, sort_keys=True) + "\n" for x in rows))
        self.ledger.chmod(0o600)

    def test_pidfd_cleanup_signals_only_exact_owned_child(self):
        proc, marker, rec = self.child()
        self.write(rec)
        self.assertTrue(process_guard.signal_record(process_guard.read_ledger(self.ledger)[0], self.config))
        proc.wait(timeout=3)
        self.assertEqual(marker.read_text(), "TERM")

    def test_stale_or_foreign_identity_variants_never_signal(self):
        mutations = {
            "reused starttime": lambda r: r.update(start=str(int(r["start"]) + 1)),
            "foreign uid": lambda r: r.update(uid=r["uid"] + 1),
            "wrong executable": lambda r: r.update(exe="/foreign/program"),
            "wrong exact argv": lambda r: r.update(argv="Zm9yZWlnbg=="),
            "wrong config": lambda r: r.update(config="/not/the/test/config.lua"),
            "forged launch token": lambda r: r.update(token="not-the-child-token"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                proc, marker, original = self.child()
                forged = dict(original)
                mutate(forged)
                self.write(forged)
                self.assertFalse(process_guard.signal_record(process_guard.read_ledger(self.ledger)[0], self.config))
                self.assertIsNone(proc.poll())
                self.assertFalse(marker.exists())
                proc.terminate()
                proc.wait(timeout=3)

    def test_writable_forged_ledger_entry_does_not_signal_unowned_child(self):
        proc, marker, _ = self.child(token="not-ledger-owned")
        forged = {"pid": proc.pid, "uid": os.getuid(), "start": "1", "exe": "/proc/self/exe",
                  "argv": "Zm9yZWlnbg==", "config": self.config, "token": "attacker"}
        self.write(forged)
        # Ledger is writable to its owner; malformed/forged content still cannot name ownership.
        self.assertFalse(process_guard.signal_record(process_guard.read_ledger(self.ledger)[0], self.config))
        self.assertIsNone(proc.poll())
        self.assertFalse(marker.exists())

    def test_pidfd_unavailable_fails_closed_without_kill_fallback(self):
        proc, marker, rec = self.child()
        with mock.patch.object(process_guard.os, "pidfd_open", side_effect=OSError("unsupported")), \
             mock.patch.object(process_guard.os, "kill", side_effect=AssertionError("legacy kill forbidden")):
            self.assertFalse(process_guard.signal_record(rec, self.config))
        self.assertIsNone(proc.poll())
        self.assertFalse(marker.exists())

    def test_symlink_or_wrong_mode_ledger_rejected(self):
        self.ledger.chmod(0o644)
        with self.assertRaises(ValueError):
            process_guard.read_ledger(self.ledger)
        link = self.root / "ledger-link"
        link.symlink_to(self.ledger)
        with self.assertRaises(ValueError):
            process_guard.read_ledger(link)

    def test_generated_launcher_preserves_exact_hyprland_argv_and_lua_string_roundtrip(self):
        # Mirror actual constrained production arguments, then execute only fixture launcher + fixture Hyprland.
        token = "ab" * 32
        config = str(self.root / "hyprland.lua")
        output = self.root / "argv.json"
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        fake = fake_bin / "Hyprland"
        fake.write_text("#!/usr/bin/env python3\nimport json,os,sys\njson.dump({'argv':sys.argv[1:],'aq':[os.getenv('AQ_NO_MODIFIERS'),os.getenv('AQ_DRM_DEVICES')]},open(os.environ['ARGV_OUT'],'w'))\n")
        fake.chmod(0o700)
        launcher = self.root / "launch.sh"
        aq = ["AQ_NO_MODIFIERS=1", "AQ_DRM_DEVICES=/dev/dri/card1"]
        argv = [*aq, "Hyprland", "-c", config]
        launcher.write_text(command_builders.launcher(str(self.root), token, " ".join(aq)))
        launcher.chmod(0o700)
        env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}", ARGV_OUT=str(output),
                   HYPRLAND_INSTANCE_SIGNATURE="fixture-host")
        subprocess.run([str(launcher)], env=env, check=True, timeout=3)
        self.assertEqual(json.loads(output.read_text()), {"argv": ["-c", config], "aq": ["1", "/dev/dri/card1"]})

        # Parse production Lua expression, including hostile path characters, without executing it.
        for path in (str(launcher), 'dir/with"quote\\slash\ncontrol'):
            expression = command_builders.dispatch(path)
            prefix = "hl.dsp.exec_cmd("
            decoded, end = json.JSONDecoder().raw_decode(expression[len(prefix):])
            self.assertEqual(decoded, path)
            self.assertEqual(expression[len(prefix) + end:], ', {workspace="4 silent"})')

    def test_generated_launcher_uses_physical_config_path_for_alias_directory(self):
        token = "cd" * 32
        physical = self.root / "physical"
        physical.mkdir()
        config = physical / "hyprland.lua"
        config.write_text("-- fixture only\n")
        alias = self.root / "alias"
        alias.symlink_to(physical, target_is_directory=True)
        fake_bin = self.root / "canonical-bin"
        fake_bin.mkdir()
        output = self.root / "canonical-argv.json"
        fake = fake_bin / "Hyprland"
        fake.write_text("#!/usr/bin/env python3\nimport json,sys\njson.dump(sys.argv[1:],open(__import__('os').environ['ARGV_OUT'],'w'))\n")
        fake.chmod(0o700)
        launcher = self.root / "canonical-launch.sh"
        launcher.write_text(command_builders.launcher(str(alias), token, "AQ_NO_MODIFIERS=1"))
        launcher.chmod(0o700)
        env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}", ARGV_OUT=str(output))
        subprocess.run([str(launcher)], env=env, check=True, timeout=3)
        self.assertEqual(json.loads(output.read_text()), ["-c", str(config)])
        self.assertEqual(os.path.realpath(config), str(config))

    def test_guard_rejects_different_config_argument(self):
        ready = self.root / "hyprland-ready"
        other = self.root / "different.lua"
        other.write_text("-- not expected\n")
        token = "ef" * 32
        code = f"import pathlib,signal,sys,time; signal.signal(signal.SIGTERM,lambda *_:sys.exit(0)); pathlib.Path({str(ready)!r}).write_text('ready'); time.sleep(60)"
        proc = subprocess.Popen(["Hyprland", "-c", code, "-c", str(other)], executable=sys.executable,
                                env=dict(os.environ, HN_LIVE_OWNER_TOKEN=token))
        self.children.append(proc)
        for _ in range(200):
            if ready.exists():
                break
            time.sleep(.005)
        self.assertTrue(ready.exists(), "fixture compositor failed to start")
        with self.assertRaisesRegex(ValueError, "exact nested config"):
            process_guard.snapshot(proc.pid, self.config, token)

    def test_run_record_failure_refuses_without_numeric_pid_signal(self):
        rig=self.root/'run-rig'
        rig.mkdir(mode=0o700)
        rig.chmod(0o700)
        control=self.root/'app-control'
        control.mkdir()
        evidence=self.root/'run-evidence'
        evidence.mkdir(mode=0o700)
        lifecycle=Path(__file__).parent/'lifecycle.py'
        session=Path(subprocess.run([sys.executable,str(lifecycle),'init',str(evidence),str(rig),str(os.getpid())],
                                    text=True,capture_output=True,check=True).stdout.strip())
        subprocess.run([sys.executable,str(lifecycle),'config',str(session)],input='-- fixture config\n',
                       text=True,check=True,capture_output=True)
        token=(session/'owner-token').read_text().strip()
        config=session/'hyprland.lua'
        digest=json.loads((session/'state.json').read_text())['config_sha256']
        compositor_code=("import pathlib,signal,sys,time; "+
                         f"pathlib.Path({str(control/'compositor-ready')!r}).write_text('ready'); "+
                         f"signal.signal(signal.SIGTERM,lambda *_:(pathlib.Path({str(control/'compositor-term')!r}).write_text('TERM'),sys.exit(0))); "+
                         "time.sleep(60)")
        compositor=subprocess.Popen([sys.executable,'-c',compositor_code,'-c',str(config)],
                                    env=dict(os.environ,HN_LIVE_OWNER_TOKEN=token))
        self.children.append(compositor)
        for _ in range(200):
            if (control/'compositor-ready').exists(): break
            time.sleep(.005)
        self.assertTrue((control/'compositor-ready').exists(),'fixture compositor failed to start')
        expected_exe=os.path.realpath(sys.executable)
        expected_argv=[sys.executable,'-c',compositor_code,'-c',str(config)]
        request=json.dumps({'argv':expected_argv,'config_sha256':digest},separators=(',',':'))
        subprocess.run([sys.executable,str(lifecycle),'intent',str(session),'compositor',expected_exe],
                       input=request,text=True,check=True,capture_output=True)
        subprocess.run([sys.executable,str(lifecycle),'record',str(session),'compositor',str(compositor.pid)],
                       text=True,check=True,capture_output=True)
        (rig/'env').write_text(f'export HOME=/tmp\nexport XDG_RUNTIME_DIR={self.root}\n')
        fake_bin=self.root/'run-bin'
        fake_bin.mkdir()
        python=fake_bin/'python3'
        python.write_text(f'''#!/bin/sh
        if [ "${{1##*/}}" = lifecycle.py ] && [ "${{2:-}}" = record ] && [ "${{4:-}}" = app ]; then
  echo rejected-app-record >>"{control}/record-attempts"
  exit 1
fi
exec /usr/bin/python3 "$@"
''')
        python.chmod(0o700)
        sleep=fake_bin/'sleep'
        sleep.write_text('#!/bin/sh\ncase "$1" in 0.01) exit 0 ;; esac\nexec /usr/bin/sleep "$@"\n')
        sleep.chmod(0o700)
        app=fake_bin/'fake-app'
        app.write_text(f'''#!/bin/bash
exec >/dev/null 2>&1
echo "$BASHPID" >"{control}/app-pid"
trap 'echo TERM >"{control}/received-term"; exit 0' TERM
while [ ! -e "{control}/allow-exit" ]; do /usr/bin/sleep .01; done
echo exited >"{control}/self-exited"
''')
        app.chmod(0o700)
        env=dict(os.environ,PATH=f"{fake_bin}:{os.environ['PATH']}",HN_NESTED_DIR=str(rig),
                 XDG_RUNTIME_DIR=str(self.root),HN_NESTED_HOST_SIG='fixture-host',
                 HYPRLAND_INSTANCE_SIGNATURE='fixture-host',WAYLAND_DISPLAY='fixture-wayland',
                 HN_LIVE_EVIDENCE_DIR=str(evidence),HN_LIVE_SESSION_DIR=str(session),
                 HN_LIVE_INVOCATION_OWNER_PID=str(os.getpid()))
        nested_source=Path(os.environ.get("HN_TEST_SOURCE_DIR",Path(__file__).parent))/'nested_hypr.sh'
        result=subprocess.run([str(nested_source),'run',str(app)],env=env,
                              text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
        app_pid=int((control/'app-pid').read_text()) if (control/'app-pid').exists() else None
        (control/'allow-exit').touch()
        deadline=time.monotonic()+3
        while app_pid is not None and not (control/'self-exited').exists() and time.monotonic()<deadline:
            time.sleep(.02)
        self.assertTrue((control/'record-attempts').is_file())
        self.assertFalse((control/'received-term').exists(),'identity-rejected app received numeric-PID fallback signal')
        self.assertEqual(result.returncode,2,result.stdout+result.stderr)
        state=json.loads((session/'state.json').read_text())
        self.assertEqual(state['phase'],'PENDING','unverified app intent was not retained')
        self.assertEqual(state['pending']['kind'],'app')
        self.assertEqual([r['kind'] for r in state['records']],['compositor'],'trusted compositor record was lost')
        self.assertTrue((session/state['records'][0]['name']).is_file())
        self.assertTrue((session/'hyprland.lua').is_file() and (session/'owner-token').is_file(),
                        'durable private config/token evidence was lost')

    def test_startup_identity_failure_is_safety_abort_not_retryable_failure(self):
        self.run_post_dispatch_failure_case("identity_rejected")

    def test_matrix_stops_after_first_safety_exit_without_second_pass(self):
        source = Path(os.environ.get("HN_TEST_SOURCE_DIR", Path(__file__).parent))
        fixture = self.root / "matrix-fixture"
        live = fixture / "tests/live"
        (live / "checks").mkdir(parents=True)
        shutil.copy(source / "matrix.sh", live)
        shutil.copy(source / "preflight.sh", live)
        shutil.copy(source / "lifecycle.py", live)
        shutil.copy(source / "process_guard.py", live)
        count = self.root / "checks.log"
        for check, code in (("rules_applied", 2), ("float_fallback", 0)):
            path = live / "checks" / f"{check}.sh"
            path.write_text(f"#!/bin/sh\necho {check} >>{shlex.quote(str(count))}\nexit {code}\n")
            path.chmod(0o700)
        fake_bin = self.root / "matrix-bin"
        fake_bin.mkdir()
        hyprctl = fake_bin / "hyprctl"
        hyprctl.write_text("#!/bin/sh\ncase \"$*\" in '-j activeworkspace') echo '{\"id\":1}';; '-j workspaces') echo '[{\"id\":4}]';; esac\n")
        hyprctl.chmod(0o700)
        matrix_evidence = self.root / "matrix-evidence"
        matrix_evidence.mkdir(mode=0o700)
        env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}", TMPDIR=str(self.root),
                   HN_LIVE_EVIDENCE_DIR=str(matrix_evidence),
                   HN_LIVE_RESERVE_BYTES="1024", HN_LIVE_RESERVE_FILES="2",
                   HN_NESTED_HOST_SIG="fixture-host", HYPRLAND_INSTANCE_SIGNATURE="fixture-host")
        result = subprocess.run(["bash", str(live / "matrix.sh")], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(count.read_text().splitlines(), ["rules_applied"])
        self.assertNotIn("RUN  float_fallback", result.stdout)
        session_dirs=list(matrix_evidence.glob("session-*"))
        self.assertEqual(len(session_dirs),1,"matrix did not retain one durable session record")
        state=json.loads((session_dirs[0]/"state.json").read_text())
        self.assertEqual(state["phase"],"CLEAN","status-2 check lacks finalized durable evidence")
        self.assertTrue((session_dirs[0]/"receipt.json").is_file(),"clean receipt missing after no-launch proof")
        self.assertFalse(list(self.root.glob("hyprnotes-task6-*/")),
                         "controller should remove scratch only after clean no-launch receipt")

    def run_post_dispatch_failure_case(self, scenario):
        source = Path(os.environ.get("HN_TEST_SOURCE_DIR", Path(__file__).parent))
        fixture = self.root / f"production-chain-{scenario}"
        live = fixture / "tests/live"
        checks = live / "checks"
        checks.mkdir(parents=True)
        for name in ("matrix.sh", "nested_hypr.sh", "preflight.sh", "lifecycle.py", "process_guard.py",
                     "command_builders.py", "placement_gate.py"):
            shutil.copy(source / name, live / name)
        for name in ("common.sh", "rules_applied.sh"):
            shutil.copy(source / "checks" / name, checks / name)
        control = self.root / f"chain-control-{scenario}"
        control.mkdir()
        second_check = control / "second-check-invoked"
        second = checks / "float_fallback.sh"
        second.write_text(f"#!/bin/sh\nprintf invoked >{shlex.quote(str(second_check))}\nexit 0\n")
        second.chmod(0o700)
        fake_bin = self.root / f"chain-bin-{scenario}"
        fake_bin.mkdir()
        python = fake_bin / "python3"
        python.write_text("""#!/bin/sh
if [ \"${1##*/}\" = lifecycle.py ] && [ \"$2\" = config ] && [ \"$FAILURE\" = marker_write_failure ]; then
  echo blocked >>\"$CONTROL/marker-write\"
  exit 1
fi
if [ \"${1##*/}\" = lifecycle.py ] && [ \"$2\" = record ]; then
  echo record >>\"$CONTROL/guard-operations\"
  if [ \"$FAILURE\" = identity_rejected ]; then
    echo 'fixture: identity rejected' >&2
    exit 1
  fi
  if [ \"$FAILURE\" = partial_ledger ]; then
    /usr/bin/python3 - \"$3\" <<'PY'
import json,os,sys
session=sys.argv[1]
state=json.load(open(os.path.join(session,'state.json')))
name='record-'+state['pending']['id']+'.json'
fd=os.open(os.path.join(session,name),os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
os.write(fd,b'{\"pid\":')
os.close(fd)
PY
    echo partial >>\"$CONTROL/partial-write\"
    exit 1
  fi
fi
exec /usr/bin/python3 \"$@\"
""")
        python.chmod(0o700)
        mktemp = fake_bin / "mktemp"
        mktemp.write_text("""#!/bin/sh
case \"$*\" in
  *startup-ownership*)
    if [ \"$FAILURE\" = marker_write_failure ]; then echo blocked >>\"$CONTROL/marker-write\"; exit 1; fi
    ;;
esac
exec /usr/bin/mktemp \"$@\"
""")
        mktemp.chmod(0o700)
        sleep = fake_bin / "sleep"
        sleep.write_text("""#!/bin/sh
case \"$1\" in 0.01|0.05) exit 0 ;; esac
exec /usr/bin/sleep \"$@\"
""")
        sleep.chmod(0o700)
        hyprctl = fake_bin / "hyprctl"
        hyprctl.write_text("""#!/bin/bash
case \"$1\" in
  -j) case \"$2\" in
    activeworkspace) echo '{\"id\":1}' ;;
    workspaces) echo '[{\"id\":4,\"name\":\"4\"}]' ;;
    clients) echo '[]' ;;
  esac ;;
  dispatch)
    echo \"$2\" >>\"$CONTROL/dispatches\"
    path=$(/usr/bin/python3 - \"$2\" <<'PY'
import json,sys
s=sys.argv[1]
print(json.JSONDecoder().raw_decode(s[s.index('(')+1:])[0])
PY
)
    \"$path\" &
    rig=$(dirname \"$path\")
    if [ \"$FAILURE\" = missing_pid ]; then
      for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s \"$CONTROL/launch-pids\" ] && break; /usr/bin/sleep .01; done
      rm -f \"$rig/hypr.pid\"
    fi
    echo ok
    ;;
  *) echo '[]' ;;
esac
""")
        hyprctl.chmod(0o700)
        fake_hyprland = fake_bin / "Hyprland"
        fake_hyprland.write_text("""#!/bin/bash
echo \"$BASHPID\" >>\"$CONTROL/launch-pids\"
while [ ! -e \"$CONTROL/allow-exit\" ]; do /usr/bin/sleep 0.02; done
echo done >>\"$CONTROL/exits\"
""")
        fake_hyprland.chmod(0o700)
        for name in ("fake-app", "fake-driver"):
            binary = fake_bin / name
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o700)
        evidence = self.root / f"chain-evidence-{scenario}"
        evidence.mkdir(mode=0o700)
        env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}", CONTROL=str(control), FAILURE=scenario,
                   TMPDIR=str(fixture), XDG_RUNTIME_DIR=str(fixture),
                   HN_LIVE_EVIDENCE_DIR=str(evidence),
                   HYPRLAND_INSTANCE_SIGNATURE="offline-host", WAYLAND_DISPLAY="offline-wayland",
                   HN_LIVE_RESERVE_BYTES="1024", HN_LIVE_RESERVE_FILES="2",
                   HN_APP_BIN=str(fake_bin / "fake-app"), HN_LIVE_DRIVER_BIN=str(fake_bin / "fake-driver"))
        result = subprocess.run(["bash", str(live / "matrix.sh"), "rules_applied", "float_fallback"],
                                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)

        launches = control / "launch-pids"
        launch_count = len(launches.read_text().splitlines()) if launches.exists() else 0
        (control / "allow-exit").touch()
        deadline = time.monotonic() + 4
        while launch_count and not (control / "exits").exists() and time.monotonic() < deadline:
            time.sleep(.02)
        if launch_count:
            self.assertEqual(len((control / "exits").read_text().splitlines()), launch_count,
                             "test-owned fake compositor did not self-exit")
            fixture_pid=int(launches.read_text().splitlines()[0])
            reaped=False
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                try:
                    got,_=os.waitpid(fixture_pid,os.WNOHANG)
                    if got==fixture_pid:
                        reaped=True
                        break
                except ChildProcessError:
                    reaped=True
                    break
                time.sleep(.02)
            self.assertTrue(reaped,"fixture creator did not reap launched test compositor")

        self.assertEqual(result.returncode, 2,
                         f"{scenario}: exit={result.returncode}, launches={launch_count}, next={second_check.exists()}\n"
                         f"{result.stdout}{result.stderr}")
        expected_launches = 0 if scenario == "marker_write_failure" else 1
        self.assertEqual(launch_count, expected_launches)
        dispatches = control / "dispatches"
        self.assertEqual(len(dispatches.read_text().splitlines()) if dispatches.exists() else 0, expected_launches)
        self.assertFalse(second_check.exists())
        if scenario == "marker_write_failure":
            self.assertIn("INCONCLUSIVE rules_applied: check declined unsafe continuation", result.stdout)
        else:
            self.assertIn("INCONCLUSIVE rules_applied: controller finalization or receipt verification failed", result.stdout)
        self.assertNotIn("RUN  float_fallback", result.stdout)
        operations = (control / "guard-operations").read_text().splitlines() if (control / "guard-operations").exists() else []
        self.assertNotIn("stop", operations, "untrusted startup must never enter guard stop/signal path")

        preserved = list(fixture.glob("hyprnotes-task6-*/"))
        session_dirs=list(evidence.glob("session-*"))
        self.assertEqual(len(session_dirs),1,"durable session not retained")
        session=session_dirs[0]
        state=json.loads((session/"state.json").read_text())
        if scenario == "marker_write_failure":
            self.assertTrue((control / "marker-write").exists())
            self.assertEqual(launch_count,0)
            self.assertEqual(state["phase"],"CLEAN","pre-dispatch write failure lacks clean no-launch proof")
            self.assertTrue((session/"receipt.json").is_file())
            self.assertTrue((session/"hyprland.lua").exists() is False)
            self.assertFalse(list(fixture.glob("hyprnotes-task6-*/")),"verified no-launch cleanup left scratch")
            return

        self.assertEqual(len(preserved), 1, "post-dispatch failure scratch was deleted or duplicated")
        task_dir = preserved[0]
        rig = task_dir
        self.assertEqual(state["phase"],"PENDING",f"{scenario}: launch intent/pending state not retained")
        self.assertEqual(state["pending"]["kind"],"compositor")
        self.assertEqual(state["records"],[],"unverified compositor was incorrectly committed")
        self.assertFalse((session/"receipt.json").exists(),"unsafe pending session received clean receipt")
        self.assertTrue((session / "owner-token").is_file())
        self.assertFalse((task_dir / "startup-ownership-untrusted").exists(),
                         "legacy scratch marker should be replaced by durable pending state")
        if scenario == "missing_pid":
            self.assertFalse((rig / "hypr.pid").exists())
        else:
            self.assertTrue((rig / "hypr.pid").is_file())
        if scenario == "partial_ledger":
            partial=list(session.glob("record-*.json"))
            self.assertEqual(len(partial),1,"record-boundary failure did not retain partial record bytes")
            self.assertTrue(partial[0].read_bytes().startswith(b'{"pid":'))
            self.assertTrue((control / "partial-write").exists())
        expected_ops=set() if scenario=="missing_pid" else {"record"}
        self.assertEqual(set(operations),expected_ops)
        stop_env = dict(env, HN_NESTED_DIR=str(rig), HN_NESTED_HOST_SIG="offline-host",
                        HN_LIVE_SESSION_DIR=str(session),HN_LIVE_INVOCATION_OWNER_PID=str(os.getpid()))
        stop = subprocess.run([str(live / "nested_hypr.sh"), "stop"], env=stop_env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(stop.returncode, 2, f"{scenario}: unsafe stop accepted: {stop.stdout} {stop.stderr}")
        self.assertEqual(json.loads((session/"state.json").read_text())["phase"],"PENDING",
                         "refused stop erased controller pending intent")
        operations_after_stop = ((control / "guard-operations").read_text().splitlines()
                                 if (control / "guard-operations").exists() else [])
        self.assertNotIn("stop", operations_after_stop, "refused stop entered guard signal path")

    def test_all_post_dispatch_failures_use_untrusted_startup_protocol(self):
        for scenario in ("missing_pid", "identity_rejected", "partial_ledger", "marker_write_failure"):
            with self.subTest(scenario=scenario):
                self.run_post_dispatch_failure_case(scenario)

    def run_committed_cleanup_chain_case(self, scenario):
        source = Path(os.environ.get("HN_TEST_SOURCE_DIR", Path(__file__).parent))
        fixture = self.root / f"committed-cleanup-{scenario}"
        live = fixture / "tests/live"
        checks = live / "checks"
        checks.mkdir(parents=True)
        for name in ("matrix.sh", "nested_hypr.sh", "preflight.sh", "lifecycle.py", "process_guard.py",
                     "command_builders.py", "placement_gate.py"):
            shutil.copy(source / name, live / name)
        shutil.copy(source / "checks/common.sh", checks / "common.sh")
        shutil.copy(source / "checks/rules_applied.sh", checks / "rules_applied.sh")
        fake_driver=live/"task6_driver_checks.py"
        fake_driver.write_text("""#!/usr/bin/env python3
import json,os,pathlib,subprocess,sys,time
result=subprocess.run([sys.argv[2],'hyprctl','-j','monitors'],check=True,text=True,capture_output=True)
rows=json.loads(result.stdout)
assert rows and rows[0]['name']=='fixture-monitor', rows
if os.environ['SCENARIO']=='already_exited':
    control=pathlib.Path(os.environ['CONTROL'])
    (control/'allow-self-exit').touch()
    deadline=time.monotonic()+2
    while not (control/'self-exited').exists() and time.monotonic()<deadline: time.sleep(.01)
print('PASS cleanup_probe: nested monitor IPC answered')
""")
        fake_driver.chmod(0o700)
        second_check = checks / "float_fallback.sh"
        second_check.write_text("#!/bin/sh\necho invoked >\"$CONTROL/second-check\"\nexit 0\n")
        second_check.chmod(0o700)
        control = self.root / f"committed-control-{scenario}"
        control.mkdir()
        runtime = fixture / "runtime"
        runtime.mkdir(mode=0o700)
        fake_bin = self.root / f"committed-bin-{scenario}"
        fake_bin.mkdir()
        python = fake_bin / "python3"
        python.write_text("#!/bin/sh\nexec /usr/bin/python3 \"$@\"\n")
        python.chmod(0o700)
        sitecustomize=live/"sitecustomize.py"
        sitecustomize.write_text("""import os
if os.environ.get('SCENARIO') in ('readiness_refused','mapping_refused','metadata_write_failure'):
    with open(os.path.join(os.environ['CONTROL'],'sitecustomize-loaded'),'a') as out: out.write('loaded\\n')
    import process_guard
    def fixture_refusal(record, config, sig=None):
        control=os.environ['CONTROL']
        with open(os.path.join(control,'fixture-refusal-called'),'a') as out: out.write('called\\n')
        with open(os.path.join(control,'guard-stop-status'),'a') as out: out.write('0\\n')
        with open(os.path.join(control,'guard-stop-all.out'),'a') as out: out.write('refused\\n')
        return False
    process_guard.signal_record=fixture_refusal
""")
        sleep = fake_bin / "sleep"
        sleep.write_text("#!/bin/sh\ncase \"$1\" in 0.01|0.05|0.1|1) exit 0 ;; esac\nexec /usr/bin/sleep \"$@\"\n")
        sleep.chmod(0o700)
        seq = fake_bin / "seq"
        seq.write_text("#!/bin/sh\nif [ \"$#\" = 2 ] && [ \"$1\" = 1 ] && [ \"$2\" = 1200 ]; then printf '1\\n2\\n'; else exec /usr/bin/seq \"$@\"; fi\n")
        seq.chmod(0o700)
        hyprctl = fake_bin / "hyprctl"
        hyprctl.write_text("""#!/usr/bin/python3
import json,os,pathlib,subprocess,sys
control=pathlib.Path(os.environ['CONTROL'])
rig=pathlib.Path(os.environ.get('HN_NESTED_DIR','/nonexistent'))
scenario=os.environ['SCENARIO']
args=sys.argv[1:]
if args[:1] == ['dispatch']:
    command=args[1]
    launcher=json.JSONDecoder().raw_decode(command[command.index('(')+1:])[0]
    child=subprocess.Popen([launcher], start_new_session=True)
    (control/'launch-pid').write_text(str(child.pid))
    with (control/'launch-pids').open('a') as stream: stream.write(str(child.pid)+'\\n')
    print('ok')
elif args[:1] == ['-j'] and len(args)>1:
    query=args[1]
    if query == 'activeworkspace': print('{"id":1}')
    elif query == 'workspaces': print('[{"id":4,"name":"4"}]')
    elif query == 'monitors': print('[{"name":"fixture-monitor"}]')
    elif query == 'clients':
        signature=os.environ.get('HYPRLAND_INSTANCE_SIGNATURE','')
        if signature == 'fixture-instance':
            print('[{"name":"fixture-monitor"}]')
        else:
            calls=control/'host-client-calls'
            count=int(calls.read_text())+1 if calls.exists() else 1
            calls.write_text(str(count))
            pid_path=rig/'hypr.pid'
            pid=int(pid_path.read_text()) if pid_path.exists() else 0
            failure_mode='mapping_refused' if scenario == 'metadata_write_failure' else scenario
            if scenario == 'readiness_refused' or failure_mode == 'mapping_refused' and count == 1:
                print('[]')
            else:
                workspace=2 if failure_mode == 'mapping_refused' else 4
                print(json.dumps([{'address':'0xfake','pid':pid,'class':'aquamarine','workspace':{'id':workspace}}]))
    else: print('[]')
else: print('ok')
""")
        hyprctl.chmod(0o700)
        fake_hyprland = fake_bin / "Hyprland"
        fake_source = self.root / "fake-hyprland.c"
        fake_source.write_text(r'''#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t term_seen;
static void on_term(int sig) { (void)sig; term_seen=1; }
static void put(const char *p,const char *s) { FILE *f=fopen(p,"w"); if(f){fputs(s,f);fclose(f);} }
static void mkdirs(char *p) { for(char *q=p+1;*q;q++) if(*q=='/'){*q=0;mkdir(p,0700);*q='/';} mkdir(p,0700); }
int main(void) {
 const char *c=getenv("CONTROL"),*r=getenv("XDG_RUNTIME_DIR"); char p[4096],d[4096];
 if(!c||!r) return 90;
 snprintf(d,sizeof d,"%s/hypr/fixture-instance",r); mkdirs(d);
 snprintf(p,sizeof p,"%s/hyprland.lock",d); FILE *f=fopen(p,"w"); if(f){fprintf(f,"%ld\nwayland-fixture\n",(long)getpid());fclose(f);}
 snprintf(p,sizeof p,"%s/wayland-fixture",r); put(p,""); signal(SIGTERM,on_term);
 for(;;) { if(term_seen){snprintf(p,sizeof p,"%s/received-term",c);put(p,"TERM");return 0;}
  snprintf(p,sizeof p,"%s/allow-self-exit",c); if(access(p,F_OK)==0){snprintf(p,sizeof p,"%s/self-exited",c);put(p,"yes");return 0;}
  struct timespec t={0,10000000}; nanosleep(&t,0); }
}''')
        subprocess.run(["cc", "-o", str(fake_hyprland), str(fake_source)], check=True, capture_output=True)
        fake_hyprland.chmod(0o700)
        for name in ("fake-app", "fake-driver"):
            binary = fake_bin / name
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o700)
        evidence = self.root / f"committed-evidence-{scenario}"
        evidence.mkdir(mode=0o700)
        env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}", CONTROL=str(control), SCENARIO=scenario,
                   TMPDIR=str(fixture), XDG_RUNTIME_DIR=str(runtime), HYPRLAND_INSTANCE_SIGNATURE="offline-host",
                   HN_LIVE_EVIDENCE_DIR=str(evidence),
                   WAYLAND_DISPLAY="offline-wayland", HN_LIVE_RESERVE_BYTES="1024", HN_LIVE_RESERVE_FILES="2",
                   HN_APP_BIN=str(fake_bin/"fake-app"), HN_LIVE_DRIVER_BIN=str(fake_bin/"fake-driver"),
                   PYTHONPATH=str(live))
        host_probe=subprocess.run(["hyprctl","-j","activeworkspace"],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.assertEqual(host_probe.stdout.strip(),'{"id":1}',f"fixture host command failed: {host_probe.stderr}")
        requested_checks=["rules_applied"] if scenario == "happy" else ["rules_applied","float_fallback"]
        try:
            result = subprocess.run(["bash", str(live/"matrix.sh"), *requested_checks], env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        except subprocess.TimeoutExpired:
            (control/'allow-self-exit').touch()
            raise
        pid = int((control/'launch-pid').read_text()) if (control/'launch-pid').exists() else None
        (control/'allow-self-exit').touch()
        deadline=time.monotonic()+4
        reaped = pid is None
        while pid is not None and time.monotonic()<deadline:
            try:
                got,_=os.waitpid(pid,os.WNOHANG)
                if got==pid:
                    reaped=True
                    break
            except ChildProcessError:
                reaped=True
                break
            time.sleep(.02)
        self.assertTrue(reaped,'fixture creator failed to reap its known fake compositor child')
        task_dirs = list(fixture.glob("hyprnotes-task6-*/"))
        evidence_root=os.environ.get("HN_TEST_EVIDENCE_DIR")
        if evidence_root:
            evidence=Path(evidence_root)/scenario
            evidence.mkdir(parents=True,exist_ok=True)
            (evidence/'matrix.stdout').write_text(result.stdout)
            (evidence/'matrix.stderr').write_text(result.stderr)
            for name in ('guard-stop-all.out','guard-stop-status','guard-stop.err','launch-pids','received-term','self-exited'):
                item=control/name
                if item.is_file(): shutil.copy(item,evidence/name)
            if task_dirs:
                shutil.copytree(task_dirs[0],evidence/'retained-task-dir')

        if scenario == 'happy':
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('PASS cleanup_probe: nested monitor IPC answered', result.stdout)
            self.assertTrue((control/'received-term').is_file(), 'normal cleanup did not signal exact owned child')
            self.assertEqual(len((control/'launch-pids').read_text().splitlines()),1)
            sessions=list(evidence.glob('session-*'))
            self.assertEqual(len(sessions),1)
            state=json.loads((sessions[0]/'state.json').read_text())
            receipt=json.loads((sessions[0]/'receipt.json').read_text())
            self.assertEqual(state['phase'],'CLEAN')
            self.assertEqual(receipt['state'],'CLEAN')
            self.assertEqual(len(receipt['exits']),1,'controller receipt lacks positive exit proof')
            self.assertEqual(task_dirs, [], 'successful guarded cleanup retained task scratch')
            self.assertFalse((control/'second-check').exists())
            return

        if scenario == 'already_exited':
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertTrue((control/'self-exited').is_file())
            self.assertFalse((control/'received-term').exists(), 'already-exited fixture must not receive signal')
            self.assertEqual(len((control/'launch-pids').read_text().splitlines()),1)
            clean_sessions=[p for p in evidence.glob('session-*')
                            if json.loads((p/'state.json').read_text())['phase']=='CLEAN']
            recorded=[p for p in clean_sessions if json.loads((p/'state.json').read_text())['records']]
            self.assertTrue(recorded,'already-exited owner lacks committed durable record')
            receipt=json.loads((recorded[0]/'receipt.json').read_text())
            self.assertTrue(receipt['exits'],'already-exited cleanup lacks positive exit proof')
            self.assertEqual(task_dirs, [], 'positive exit proof should allow normal metadata cleanup')
            return

        if scenario in ('readiness_refused','mapping_refused','metadata_write_failure'):
            self.assertTrue((control/'sitecustomize-loaded').is_file(),'fixture refusal hook did not load')
            self.assertTrue((control/'fixture-refusal-called').is_file(),'controller did not reach guarded signal boundary')
        self.assertEqual(result.returncode, 2,
                         f"{scenario}: matrix exit={result.returncode}, taskdirs={task_dirs}\n{result.stdout}{result.stderr}")
        self.assertNotIn('RUN  float_fallback', result.stdout)
        self.assertFalse((control/'second-check').exists())
        self.assertEqual(len((control/'launch-pids').read_text().splitlines()),1,
                         'postcommit cleanup refusal must prohibit another compositor launch')
        refusals=(control/'guard-stop-status').read_text().splitlines()
        self.assertGreaterEqual(len(refusals),2,'common and matrix must independently request finish')
        self.assertTrue(all(row=='0' for row in refusals),'fixture guard refusal boundary returned unexpected status')
        self.assertEqual((control/'guard-stop-all.out').read_text().splitlines(),['refused']*len(refusals))
        self.assertEqual(len(task_dirs), 1, 'failed guarded cleanup removed recovery scratch')
        rig=task_dirs[0]
        session_dirs=list(evidence.glob('session-*'))
        self.assertEqual(len(session_dirs),1,'cleanup refusal lost durable session')
        state=json.loads((session_dirs[0]/'state.json').read_text())
        self.assertEqual(state['phase'],'ACTIVE')
        self.assertEqual([r['kind'] for r in state['records']],['compositor'])
        self.assertTrue((session_dirs[0]/state['records'][0]['name']).is_file())
        self.assertTrue((session_dirs[0]/'owner-token').is_file())
        self.assertTrue((session_dirs[0]/'hyprland.lua').is_file())
        self.assertFalse((session_dirs[0]/'receipt.json').exists(),'guard refusal must not commit clean receipt')
        self.assertFalse((control/'received-term').exists(), 'guard refusal must not be interpreted as clean')
        if scenario == 'metadata_write_failure':
            self.assertEqual(state['phase'],'ACTIVE','durable state must retain cleanup-refusal metadata')

    def test_committed_readiness_and_mapping_cleanup_refusal_preserve_owner(self):
        for scenario in ('readiness_refused','mapping_refused','metadata_write_failure'):
            with self.subTest(scenario=scenario): self.run_committed_cleanup_chain_case(scenario)

    def test_valid_start_functional_check_and_guarded_cleanup(self):
        self.run_committed_cleanup_chain_case('happy')

    def test_already_exited_owner_is_confirmed_without_signal(self):
        self.run_committed_cleanup_chain_case('already_exited')

    def test_wrong_workspace_or_missing_exact_client_blocks_driver_and_only_owned_cleanup(self):
        expected = {"address": "0xabc", "pid": 4102, "workspace": {"id": 2}}
        correct = {"address": "0xabc", "pid": 4102, "workspace": {"id": 4}}
        no_match = {"address": "0xdef", "pid": 4102, "workspace": {"id": 4}}
        duplicates = [dict(expected, workspace={"id": 4}), dict(expected)]
        for label, clients in (("correct workspace", [correct]), ("wrong workspace", [expected]),
                               ("no address match", [no_match]), ("no clients", []),
                               ("ambiguous match", duplicates)):
            with self.subTest(label=label):
                proc, marker, record = self.child()
                driver_actions = []
                host_dispatches = []
                cleanup = []
                workspace = placement_gate.observed_workspace(clients, "0xabc", 4102)
                if workspace == 4:
                    driver_actions.append("driver")
                else:
                    cleanup.append("owned-only" if process_guard.signal_record(record, self.config) else "refused")
                    proc.wait(timeout=3)
                    self.assertEqual(marker.read_text(), "TERM")
                self.assertEqual(driver_actions, ["driver"] if label == "correct workspace" else [])
                self.assertEqual(host_dispatches, [])  # no move, focus, or workspace switch fallback
                self.assertEqual(cleanup, [] if label == "correct workspace" else ["owned-only"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
