#!/usr/bin/env python3
"""Offline behavioral tests for the real nested_hypr.sh lifecycle adapter."""
import ctypes, json, os, pathlib, shutil, signal, subprocess, tempfile, time, unittest
HERE = pathlib.Path(__file__).resolve().parent

class NestedLifecycleTests(unittest.TestCase):
    def setUp(self):
        # Adopt and reap fixture grandchildren orphaned by fake host-command subprocesses.
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)
        self.root = pathlib.Path(tempfile.mkdtemp(prefix="hn-2a-")); os.chmod(self.root, 0o700)
        self.scratch, self.evidence, self.runtime, self.bindir = [self.root / p for p in ("scratch", "evidence", "runtime", "bin")]
        for p in (self.evidence, self.runtime, self.bindir): p.mkdir(mode=0o700)
        self.events, self.log = self.root / "events", self.root / "compositor.json"
        self.signals = self.root / "signals"
        self.fixture_reaped = []
        self.fake = self.bindir / "Hyprland"
        cfile = self.root / "fake.c"
        cfile.write_text(r'''#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static int signal_fd=-1;
static void on_term(int sig){const char s[]="SIGTERM\n";if(signal_fd>=0)write(signal_fd,s,sizeof(s)-1);_exit(0);}
int main(int n,char**v){const char*l=getenv("HN_FIXTURE_LOG"),*r=getenv("XDG_RUNTIME_DIR");signal_fd=open(getenv("HN_FIXTURE_SIGNALS"),O_CREAT|O_WRONLY|O_APPEND,0600);signal(SIGTERM,on_term);FILE*f=fopen(l,"w");if(!f)return 91;fprintf(f,"%d",getpid());fclose(f);char d[4096],p[4096];snprintf(p,sizeof p,"%s/hypr",r);mkdir(p,0700);snprintf(d,sizeof d,"%s/hypr/fake",r);mkdir(d,0700);snprintf(p,sizeof p,"%s/hyprland.lock",d);f=fopen(p,"w");fprintf(f,"%d\nwayland-fake\n",getpid());fclose(f);snprintf(p,sizeof p,"%s/wayland-fake",r);f=fopen(p,"w");fclose(f);for(;;)pause();}''')
        subprocess.run(["cc", "-o", str(self.fake), str(cfile)], check=True, capture_output=True); self.fake.chmod(0o700)
        ctl = self.bindir / "hyprctl"
        ctl.write_text('''#!/usr/bin/python3
import json,os,pathlib,re,subprocess,sys
a=sys.argv[1:]
with open(os.environ["HN_FIXTURE_EVENTS"],"a") as f:f.write(json.dumps(a)+"\\n")
if a[:1]==["dispatch"]:
 start=a[1].find('(')+1; end=a[1].find(',',start); launcher=json.loads(a[1][start:end]); subprocess.Popen([launcher],env=os.environ.copy(),start_new_session=True)
 if os.environ.get("HN_FIXTURE_CHMOD_AFTER_DISPATCH"): os.chmod(os.environ["HN_NESTED_DIR"],0o500)
 print("ok")
elif a[:2]==["-j","workspaces"]:print('[{"id":4}]')
elif a[:2]==["-j","activeworkspace"]:print('{"id":1}')
elif a[:2]==["-j","clients"]:
 p=pathlib.Path(os.environ["HN_NESTED_DIR"])/"hypr.pid"; pid=int(p.read_text()) if p.exists() else -1; print(json.dumps([{"pid":pid,"address":"0xabc","workspace":{"id":4}}]))
elif a[:2]==["-j","monitors"]:print('[{"name":"FAKE","width":1280,"height":720}]')
else:print("ok")
'''); ctl.chmod(0o700)
        self.app = self.bindir / "fixture-app"
        app_source=self.root/"app.c"
        app_source.write_text("#include <unistd.h>\nint main(int n,char**v){usleep(250000);return n<0;}\n")
        subprocess.run(["cc","-o",str(self.app),str(app_source)],check=True,capture_output=True); self.app.chmod(0o700)
        self.env = os.environ.copy(); self.env.update({"PATH":f"{self.bindir}:{os.environ['PATH']}","HN_NESTED_DIR":str(self.scratch),"HN_LIVE_EVIDENCE_DIR":str(self.evidence),"HN_LIVE_INVOCATION_OWNER_PID":str(os.getpid()),"HN_LIVE_RESERVE_BYTES":"1","HN_LIVE_RESERVE_FILES":"1","HN_FIXTURE_EVENTS":str(self.events),"HN_FIXTURE_LOG":str(self.log),"HN_FIXTURE_SIGNALS":str(self.signals),"XDG_RUNTIME_DIR":str(self.runtime),"WAYLAND_DISPLAY":"wayland-host","HYPRLAND_INSTANCE_SIGNATURE":"host-fake","HN_NESTED_AQ":"AQ_NO_MODIFIERS=1"})
    def cli(self,*args,timeout=15): return subprocess.run([str(HERE/"nested_hypr.sh"),*args],env=self.env,text=True,capture_output=True,timeout=timeout)
    def events_list(self): return [json.loads(x) for x in self.events.read_text().splitlines()] if self.events.exists() else []
    def session(self):
        locs=list(self.evidence.glob("lookup-*.json"))
        return pathlib.Path(json.loads(locs[0].read_text())["session"]) if locs else None
    def tearDown(self):
        if self.log.exists():
            try:
                pid=int(self.log.read_text())
                try: os.kill(pid,signal.SIGTERM)
                except ProcessLookupError: pass
                reaped=False
                for _ in range(200):
                    try:
                        got,_=os.waitpid(pid,os.WNOHANG)
                        if got: reaped=True; break
                    except ChildProcessError:
                        try: os.kill(pid,0)
                        except ProcessLookupError: reaped=True; break
                    time.sleep(.01)
                if not reaped: self.fail(f"fixture compositor {pid} survived fixture cleanup")
                self.fixture_reaped.append(pid)
            except (OSError,ValueError,KeyError): pass
        if self.scratch.exists(): os.chmod(self.scratch,0o700)
        shutil.rmtree(self.root)
    def test_direct_start_commits_durable_config_and_exact_compositor_record(self):
        env=self.env.copy(); env.pop("HN_LIVE_INVOCATION_OWNER_PID")
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"start"],env=env,text=True,capture_output=True,timeout=15)
        self.assertEqual(p.returncode,0,p.stderr)
        session=self.session(); self.assertIsNotNone(session,"durable session lookup missing")
        self.assertTrue((session/"hyprland.lua").is_file()); self.assertFalse((self.scratch/"hyprland.lua").exists())
        st=json.loads((session/"state.json").read_text()); self.assertEqual(st["phase"],"ACTIVE")
        rec=json.loads((session/st["records"][0]["name"]).read_text())
        self.assertEqual(rec["exe"],str(self.fake.resolve())); self.assertEqual(rec["config"],str(session/"hyprland.lua"))
        import base64
        self.assertEqual(base64.b64decode(rec["argv"]).split(b"\0")[:-1],
                         [str(self.fake).encode(),b"-c",str(session/"hyprland.lua").encode()])
        self.assertEqual((session/"owner-token").stat().st_mode&0o777,0o600)
        self.assertEqual(rec["token"],(session/"owner-token").read_text().strip())
        self.assertEqual(st["owner_pid"],os.getpid())
    def test_unrecorded_dispatch_retains_pending_and_never_retries(self):
        ctl=self.bindir/"hyprctl"; ctl.write_text('''#!/usr/bin/python3
+import json,os,sys
+a=sys.argv[1:]
+with open(os.environ["HN_FIXTURE_EVENTS"],"a") as f:f.write(json.dumps(a)+"\\n")
+if a[:1]==["dispatch"]:print("ok")
+elif a[:2]==["-j","workspaces"]:print('[{"id":4}]')
+elif a[:2]==["-j","activeworkspace"]:print('{"id":1}')
+elif a[:2]==["-j","clients"]:print('[]')
+'''.replace("\n+","\n")); ctl.chmod(0o700)
        p=self.cli("start",timeout=10); self.assertEqual(p.returncode,2,p.stderr)
        self.assertEqual(sum(x[:1]==["dispatch"] for x in self.events_list()),1)
        session=self.session(); self.assertIsNotNone(session)
        st=json.loads((session/"state.json").read_text()); self.assertEqual(st["phase"],"PENDING"); self.assertIsNotNone(st["pending"])
    def test_direct_session_reuse_env_status_run_and_controller_stop(self):
        p=self.cli("start"); self.assertEqual(p.returncode,0,p.stderr); session=self.session(); self.assertIsNotNone(session)
        self.assertEqual(self.cli("env").returncode,0); self.assertIn(str(session),self.cli("env").stdout)
        self.assertEqual(self.cli("status").returncode,0)
        self.assertEqual(self.cli("hyprctl","-j","monitors").returncode,0)
        p=self.cli("run",str(self.app),"--sample"); self.assertEqual(p.returncode,0,p.stderr)
        st=json.loads((session/"state.json").read_text()); self.assertEqual([r["kind"] for r in st["records"]],["compositor","app"])
        rec=json.loads((session/st["records"][1]["name"]).read_text())
        import base64
        self.assertEqual(base64.b64decode(rec["argv"]).split(b"\0")[:-1],[str(self.app).encode(),b"--sample"])
        p=self.cli("stop"); self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads((session/"state.json").read_text())["phase"],"CLEAN"); self.assertFalse(self.scratch.exists())
        self.assertEqual(self.signals.read_text().splitlines(),["SIGTERM"],"controller sends exactly one guarded signal")
    def test_owner_loss_does_not_rebind_to_transient_cli_pid(self):
        p=self.cli("start"); self.assertEqual(p.returncode,0,p.stderr); session=self.session(); self.assertIsNotNone(session)
        st=json.loads((session/"state.json").read_text())
        self.assertEqual(st["owner_pid"],os.getpid())

    def test_owner_loss_refuses_new_intent_but_controller_finishes_committed_record(self):
        owner=subprocess.Popen(["/usr/bin/sleep","60"])
        self.addCleanup(owner.wait)
        self.addCleanup(owner.terminate)
        env=self.env.copy(); env["HN_LIVE_INVOCATION_OWNER_PID"]=str(owner.pid)
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"start"],env=env,text=True,capture_output=True,timeout=15)
        self.assertEqual(p.returncode,0,p.stderr)
        session=self.session(); self.assertIsNotNone(session)
        state=json.loads((session/"state.json").read_text())
        self.assertEqual(state["owner_pid"],owner.pid)
        owner.terminate(); owner.wait(timeout=3)
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"run",str(self.app)],env=env,text=True,capture_output=True,timeout=10)
        self.assertEqual(p.returncode,2,p.stderr)
        state=json.loads((session/"state.json").read_text())
        self.assertEqual(state["phase"],"ACTIVE")
        self.assertEqual([r["kind"] for r in state["records"]],["compositor"])
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"stop"],env=env,text=True,capture_output=True,timeout=10)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads((session/"state.json").read_text())["phase"],"CLEAN")

    def test_preinitialized_direct_check_session_is_reused_without_owner_rebind(self):
        self.scratch.mkdir(mode=0o700)
        p=subprocess.run(["python3",str(HERE/"lifecycle.py"),"init",str(self.evidence),str(self.scratch),str(os.getpid())],
                         text=True,capture_output=True,check=True)
        session=pathlib.Path(p.stdout.strip())
        env=self.env.copy(); env["HN_LIVE_SESSION_DIR"]=str(session)
        result=subprocess.run([str(HERE/"nested_hypr.sh"),"start"],env=env,text=True,capture_output=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        st=json.loads((session/"state.json").read_text())
        self.assertEqual(st["owner_pid"],os.getpid())
        self.assertEqual(st["phase"],"ACTIVE")
        self.assertEqual([r["kind"] for r in st["records"]],["compositor"])
        self.assertEqual(self.session(),None,"explicit check session need not publish another lookup")

    def test_missing_or_ambiguous_session_never_reconstructs_from_scratch(self):
        p=self.cli("status")
        self.assertEqual(p.returncode,2)
        self.assertEqual(self.events_list(),[])
        self.assertEqual(list(self.evidence.iterdir()),[])
        self.scratch.mkdir(mode=0o700); sentinel=self.scratch/"stale-owner-evidence"; sentinel.write_text("retain")
        p=self.cli("start")
        self.assertEqual(p.returncode,2)
        self.assertEqual(sentinel.read_text(),"retain")
        self.assertEqual(self.events_list(),[])
        sentinel.unlink(); self.scratch.rmdir()
        p=self.cli("start"); self.assertEqual(p.returncode,0,p.stderr)
        env=self.env.copy(); env["HN_LIVE_SESSION_DIR"]=str(self.evidence/"session-forged")
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"status"],env=env,text=True,capture_output=True)
        self.assertEqual(p.returncode,2)
        env=self.env.copy(); env["HN_LIVE_SESSION_DIR"]=str(self.session()); env["HN_LIVE_INVOCATION_OWNER_PID"]=str(os.getppid())
        p=subprocess.run([str(HERE/"nested_hypr.sh"),"status"],env=env,text=True,capture_output=True)
        self.assertEqual(p.returncode,2)
        self.assertEqual(json.loads((self.session()/"state.json").read_text())["phase"],"ACTIVE")

    def test_scratch_mode_failure_after_dispatch_retains_intent_without_retry(self):
        self.env["HN_FIXTURE_CHMOD_AFTER_DISPATCH"]="1"
        p=self.cli("start"); self.assertEqual(p.returncode,2,p.stderr)
        self.assertEqual(sum(x[:1]==["dispatch"] for x in self.events_list()),1)
        session=self.session(); self.assertIsNotNone(session)
        st=json.loads((session/"state.json").read_text()); self.assertEqual(st["phase"],"PENDING")
        self.assertTrue((session/"owner-token").exists()); self.assertTrue((session/"hyprland.lua").exists())
        stop=self.cli("stop"); self.assertEqual(stop.returncode,2)
        self.assertEqual(json.loads((session/"state.json").read_text())["phase"],"PENDING")

    def test_app_record_refusal_keeps_pending_and_never_uses_pid_fallback(self):
        p=self.cli("start"); self.assertEqual(p.returncode,0,p.stderr)
        session=self.session(); self.assertIsNotNone(session)
        bad=self.bindir/"script-app"; marker=self.root/"bad-app-started"; signal_marker=self.root/"bad-app-signaled"
        mismatch=self.bindir/"mismatching-elf"; source=self.root/"mismatch.c"
        source.write_text(f'''#include <signal.h>\n#include <stdio.h>\n#include <unistd.h>\nstatic void term(int s){{FILE*f=fopen("{signal_marker}","w");if(f){{fputs("term",f);fclose(f);}}_exit(0);}}\nint main(){{signal(SIGTERM,term);FILE*f=fopen("{marker}","w");fprintf(f,"%d",getpid());fclose(f);usleep(900000);return 0;}}\n''')
        subprocess.run(["cc","-o",str(mismatch),str(source)],check=True,capture_output=True); mismatch.chmod(0o700)
        bad.write_text(f"#!/bin/sh\nexec {mismatch}\n"); bad.chmod(0o700)
        p=self.cli("run",str(bad)); self.assertEqual(p.returncode,2,p.stderr)
        self.assertTrue(marker.exists())
        st=json.loads((session/"state.json").read_text()); self.assertEqual(st["phase"],"PENDING")
        self.assertEqual([r["kind"] for r in st["records"]],["compositor"])
        self.assertFalse(signal_marker.exists(),"adapter must not signal unrecorded app by numeric PID")
        pid=int(marker.read_text())
        time.sleep(1.0)
        self.assertFalse(signal_marker.exists(),"adapter sent no signal before fixture-owned cleanup")
        try: os.kill(pid,signal.SIGTERM)  # fixture-owned cleanup only, after no-signal assertion
        except ProcessLookupError: pass
        reaped=False
        for _ in range(200):
            try:
                got,_=os.waitpid(pid,os.WNOHANG)
                if got: reaped=True; break
            except ChildProcessError:
                try: os.kill(pid,0)
                except ProcessLookupError: reaped=True; break
            time.sleep(.01)
        self.assertTrue(reaped,"fixture app cleanup did not positively complete")

    def test_hyprctl_json_stdout_has_no_owner_diagnostic_prefix(self):
        started=self.cli("start")
        self.assertEqual(started.returncode,0,started.stderr)
        result=self.cli("hyprctl","-j","monitors")
        self.assertEqual(result.returncode,0,result.stderr)
        rows=json.loads(result.stdout)
        self.assertEqual(rows,[{"name":"FAKE","width":1280,"height":720}])

    def test_owner_mismatch_still_refuses_status_without_host_command(self):
        started=self.cli("start")
        self.assertEqual(started.returncode,0,started.stderr)
        session=self.session()
        before=self.events_list()
        env=self.env.copy()
        env["HN_LIVE_SESSION_DIR"]=str(session)
        env["HN_LIVE_INVOCATION_OWNER_PID"]=str(os.getppid())
        result=subprocess.run([str(HERE/"nested_hypr.sh"),"hyprctl","-j","monitors"],
                              env=env,text=True,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,2,result.stdout+result.stderr)
        self.assertEqual(self.events_list(),before,"owner mismatch reached nested host command")
        state=json.loads((session/"state.json").read_text())
        self.assertEqual(state["phase"],"ACTIVE","owner mismatch mutated controller state")

if __name__=="__main__": unittest.main(verbosity=2)
