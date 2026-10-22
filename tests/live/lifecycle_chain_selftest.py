#!/usr/bin/env python3
"""Actual nested/common/check/matrix lifecycle integration regressions."""
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
FILES = ("matrix.sh", "nested_hypr.sh", "preflight.sh", "lifecycle.py", "process_guard.py",
         "command_builders.py", "placement_gate.py")


class LifecycleChainTests(unittest.TestCase):
    def exercise(self, mode, receipt_mode=None):
        ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)
        with tempfile.TemporaryDirectory(prefix="hn-2b2-chain-") as temp:
            root = Path(temp)
            root.chmod(0o700)
            live = root / "tests/live"
            checks = live / "checks"
            checks.mkdir(parents=True)
            for name in FILES:
                shutil.copy(HERE / name, live / name)
            for name in ("common.sh", "rules_applied.sh", "float_fallback.sh"):
                shutil.copy(HERE / "checks" / name, checks / name)
            control, evidence, runtime, bindir, scratch = [root / n for n in
                ("control", "evidence", "runtime", "bin", "scratch")]
            for directory in (control, evidence, runtime, bindir, scratch):
                directory.mkdir(mode=0o700)
            self.install_host_commands(root, live, control, bindir)
            if mode in ("bypass_common_exit", "receipt"):
                self.install_boundary_check(checks / "rules_applied.sh", mode, receipt_mode)
            if mode == "exit_one_before_started":
                self.install_prestart_failure_check(checks / "rules_applied.sh")
            if mode == "repeat_term":
                self.install_signal_check(checks / "rules_applied.sh")

            env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", CONTROL=str(control),
                       TMPDIR=str(scratch), XDG_RUNTIME_DIR=str(runtime),
                       HYPRLAND_INSTANCE_SIGNATURE="fixture-host", WAYLAND_DISPLAY="fixture-wayland",
                       HN_LIVE_EVIDENCE_DIR=str(evidence), HN_LIVE_RESERVE_BYTES="1024",
                       HN_LIVE_RESERVE_FILES="2", HN_APP_BIN=str(bindir / "fake-app"),
                       HN_LIVE_DRIVER_BIN=str(live / "fake-driver"), HN_CHAIN_MODE=mode,
                       HN_CHAIN_LIVE_DIR=str(live), HN_CHAIN_RECEIPT_MODE=receipt_mode or "")
            env["PYTHONPATH"] = str(live)
            bash_env=root/"fixture-bash-env"
            bash_env.write_text('''case "$0" in *checks/float_fallback.sh|*checks/rules_applied.sh)
  trap 'rc=$?; printf "exit=%s command=%s\\n" "$rc" "$BASH_COMMAND" >>"$CONTROL/check-error-trace"' ERR
  ;;
esac
''')
            env["BASH_ENV"]=str(bash_env)
            checks_to_run = self.check_list(mode)
            result = subprocess.run(["bash", str(live / "matrix.sh"), *checks_to_run], env=env,
                                    text=True, capture_output=True, timeout=35)
            summary = self.collect_result(root, control, evidence, result)
            app_diagnostics=[]
            for log in scratch.glob("hyprnotes-task6-*/app/*.log"):
                text=log.read_text(errors="replace")
                text=re.sub(r"[0-9a-fA-F]{64}","[REDACTED]",text)
                app_diagnostics.append({"name":log.name,"text":text})
            summary["app_diagnostics"]=app_diagnostics
            # Traces are opt-in diagnostics; never write into the source tree by default.
            trace_root=os.environ.get("HN_CHAIN_TRACE_DIR")
            if trace_root:
              trace_dir=Path(trace_root)/"2b2-traces"
              trace_dir.mkdir(mode=0o700,parents=True,exist_ok=True)
              trace_name=mode+("-"+receipt_mode if receipt_mode else "")
              (trace_dir/(trace_name+".stdout")).write_text(result.stdout)
              (trace_dir/(trace_name+".stderr")).write_text(result.stderr)
              safe_sessions=[{"phase":s["state"].get("phase"),"session":s["state"].get("session"),
                              "pending_kind":(s["state"].get("pending") or {}).get("kind"),
                              "record_kinds":[x.get("kind") for x in s["state"].get("records",[])],
                              "receipt_exists":s["receipt_exists"],
                              "receipt_session":(s["receipt"] or {}).get("session"),
                              "receipt_state":(s["receipt"] or {}).get("state")} for s in summary["sessions"]]
              (trace_dir/(trace_name+".json")).write_text(json.dumps({"status":result.returncode,
                  "launch_count":len(summary["launches"]),"term":summary["term"],
                  "signal_count":summary["signal_count"],
                  "compositor_self_exit":summary["compositor_self_exit"],"app_pids":summary["app_pids"],
                  "driver_calls":summary["driver_calls"],"finish_calls":summary["finish_calls"],
                  "refusals":summary["refusals"],"sessions":safe_sessions,
                  "fixture_events":summary["fixture_events"],"finish_errors":summary["finish_errors"],
                  "record_errors":summary["record_errors"],
                  "app_diagnostics":app_diagnostics,"check_errors":summary["check_errors"]},indent=2)+"\n")
            self.assertNoChildrenRemain(control, summary)
        self.assertFalse(root.exists(), "test-owned fixture directory remained after creator cleanup")
        summary["fixture_leftovers"] = 0
        return summary

    @staticmethod
    def check_list(mode):
        if mode == "float_both":
            return ["float_fallback"]
        if mode == "float_unsafe_first":
            return ["float_fallback", "rules_applied"]
        if mode == "window_moved":
            return ["rules_applied"]
        if mode in ("happy_next", "functional_one_next", "exit_one_before_started"):
            return ["rules_applied", "rules_applied"]
        if mode == "receipt":
            return ["rules_applied", "rules_applied"]
        return ["rules_applied", "float_fallback"]

    @staticmethod
    def install_host_commands(root, live, control, bindir):
        wrapper = bindir / "python3"
        wrapper.write_text("""#!/bin/sh
if [ "${1:-}" = "$HN_CHAIN_LIVE_DIR/lifecycle.py" ] && [ "${2:-}" = finish ]; then
  printf '%s\\n' "$3" >>"$CONTROL/finish-calls"
  /usr/bin/python3 "$@" 2>>"$CONTROL/lifecycle-finish-stderr"
  exit $?
fi
if [ "${1:-}" = "$HN_CHAIN_LIVE_DIR/lifecycle.py" ] && [ "${2:-}" = verify-clean ]; then
  /usr/bin/python3 "$@" 2>>"$CONTROL/lifecycle-verify-stderr"
  exit $?
fi
if [ "${1:-}" = "$HN_CHAIN_LIVE_DIR/lifecycle.py" ] && [ "${2:-}" = record ]; then
  /usr/bin/python3 "$@" 2>>"$CONTROL/lifecycle-record-stderr"
  exit $?
fi
exec /usr/bin/python3 "$@"
""")
        wrapper.chmod(0o700)
        # Real sleep: hn_start_app relies on it for app identity recording; a no-op sleep raced.
        (bindir / "sleep").write_text('#!/bin/sh\nexec /usr/bin/sleep "$@"\n')
        (bindir / "sleep").chmod(0o700)
        (bindir / "seq").write_text("#!/bin/sh\nprintf '1\\n2\\n'\n")
        (bindir / "seq").chmod(0o700)

        sitecustomize = live / "sitecustomize.py"
        sitecustomize.write_text("""import os
mode=os.environ.get('HN_CHAIN_MODE')
if mode in ('guard_refusal','float_unsafe_first'):
 import process_guard
 def refuse(record, config, sig=None):
  with open(os.path.join(os.environ['CONTROL'],'refusal-attempts'),'a') as f: f.write('refused\\n')
  return False
 process_guard.signal_record=refuse
""")

        hyprctl = bindir / "hyprctl"
        hyprctl.write_text(r'''#!/usr/bin/python3
import json,os,pathlib,subprocess,sys,time
control=pathlib.Path(os.environ['CONTROL'])
args=sys.argv[1:]
sig=os.environ.get('HYPRLAND_INSTANCE_SIGNATURE','')
rig=pathlib.Path(os.environ.get('HN_NESTED_DIR','/nonexistent'))
mode=os.environ.get('HN_CHAIN_MODE','')
with (control/'host-commands').open('a') as f:f.write((' '.join(args)+' sig='+sig+' rig='+str(rig.name))+'\\n')
def lines(path):
 try: return path.read_text().splitlines()
 except FileNotFoundError: return []
if args[:1]==['dispatch']:
 command=args[1]
 launcher=json.JSONDecoder().raw_decode(command[command.index('(')+1:])[0]
 child=subprocess.Popen([launcher],start_new_session=True)
 for _ in range(200):
  p=rig/'hypr.pid'
  if p.exists(): break
  time.sleep(.005)
 if p.exists():
  pid=p.read_text().strip()
  with (control/'launch-pids').open('a') as f: f.write(pid+'\n')
  (control/'launch-count').write_text(str(len(lines(control/'launch-pids'))))
 print('ok')
elif args[:1]==['-j'] and len(args)>1:
 query=args[1]
 if query=='activeworkspace': print('{"id":1}')
 elif query=='workspaces': print('[{"id":4,"name":"4"}]')
 elif query=='monitors': print('[{"name":"fixture-monitor"}]')
 elif query=='clients' and sig=='fixture-instance':
  events=lines(rig/'app/actions')
  clients=[]
  apppid=0
  try: apppid=int((rig/'app/app.pid').read_text())
  except (OSError,ValueError): pass
  if 'new-note' in events:
   clients.append({'address':'0xsticky','pid':apppid,'class':'hyprnotes','initialTitle':'hyprnotes-sticky: fixture','floating':True,'workspace':{'id':4}})
  if 'show-organizer' in events:
   floating=False
   try: floating=json.loads((rig/'app/hn-config/config.json').read_text())['windows']['organizerFloating']
   except (OSError,KeyError,ValueError): pass
   clients.append({'address':'0xorganizer','pid':apppid,'class':'hyprnotes','initialTitle':'hyprnotes-organizer: fixture','floating':floating,'workspace':{'id':4}})
  print(json.dumps(clients))
 elif query=='clients':
  launched=len(lines(control/'launch-pids'))
  if mode=='exit_one_before_started' and launched==1:
   print('[]')
  else:
   try: pid=int((rig/'hypr.pid').read_text())
   except (OSError,ValueError): pid=0
   moved=mode=='window_moved' and (control/'driver-calls').exists()
   print(json.dumps([{'address':'0xfake','pid':pid,'class':'aquamarine','workspace':{'id':5 if moved else 4}}]))
 else: print('[]')
else: print('ok')
''')
        hyprctl.chmod(0o700)

        cfile = root / "fixture-compositor.c"
        cfile.write_text(r'''#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t term_seen;
static void on_term(int sig){(void)sig;term_seen=1;}
static void put(const char *p,const char *s){FILE *f=fopen(p,"w");if(f){fputs(s,f);fclose(f);}}
static void mkdirs(char *p){for(char*q=p+1;*q;q++)if(*q=='/'){*q=0;mkdir(p,0700);*q='/';}mkdir(p,0700);}
int main(void){const char*c=getenv("CONTROL"),*r=getenv("XDG_RUNTIME_DIR");char p[4096],d[4096];
 if(!c||!r)return 90;snprintf(d,sizeof d,"%s/hypr/fixture-instance",r);mkdirs(d);
 snprintf(p,sizeof p,"%s/hyprland.lock",d);FILE*f=fopen(p,"w");if(f){fprintf(f,"%ld\nwayland-fixture\n",(long)getpid());fclose(f);}
 snprintf(p,sizeof p,"%s/wayland-fixture",r);put(p,"");signal(SIGTERM,on_term);
  for(;;){if(term_seen){snprintf(p,sizeof p,"%s/received-term",c);put(p,"TERM");snprintf(p,sizeof p,"%s/term-signals",c);FILE*t=fopen(p,"a");if(t){fputs("compositor\n",t);fclose(t);}return 0;}
  snprintf(p,sizeof p,"%s/allow-exit",c);if(access(p,F_OK)==0){snprintf(p,sizeof p,"%s/self-exited",c);put(p,"yes");return 0;}
  struct timespec t={0,10000000};nanosleep(&t,0);}}
''')
        fake_hyprland = bindir / "Hyprland"
        subprocess.run(["cc", "-o", str(fake_hyprland), str(cfile)], check=True, capture_output=True)
        fake_hyprland.chmod(0o700)

        app_source=root/"fixture-app.c"
        app_source.write_text(r'''#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t term_seen;
static void on_term(int sig){(void)sig;term_seen=1;}
static void append(const char *path,const char *value){FILE*f=fopen(path,"a");if(f){fprintf(f,"%s\n",value);fclose(f);}}
int main(int argc,char **argv){const char*c=getenv("CONTROL"),*n=getenv("HN_NESTED_DIR");char p[4096];int background=0;
 if(!c||!n)return 90;snprintf(p,sizeof p,"%s/app",n);mkdir(p,0700);
 snprintf(p,sizeof p,"%s/app/app.pid",n);FILE*f=fopen(p,"w");if(f){fprintf(f,"%ld\n",(long)getpid());fclose(f);}
 for(int i=1;i<argc;i++)if(strcmp(argv[i],"--background")==0)background=1;
 if(background){snprintf(p,sizeof p,"%s/app-pids",c);char pid[64];snprintf(pid,sizeof pid,"%ld",(long)getpid());append(p,pid);snprintf(p,sizeof p,"%s/app-actions",c);append(p,"background");
  signal(SIGTERM,on_term);while(!term_seen){snprintf(p,sizeof p,"%s/allow-exit",c);if(access(p,F_OK)==0)break;struct timespec t={0,10000000};nanosleep(&t,0);}
  snprintf(p,sizeof p,"%s/app-exited-pids",c);append(p,pid);return 0;}
 const char *action="unknown";for(int i=1;i<argc;i++){if(strcmp(argv[i],"--new-note")==0)action="new-note";if(strcmp(argv[i],"--show-organizer")==0)action="show-organizer";}
 snprintf(p,sizeof p,"%s/app-actions",c);append(p,action);snprintf(p,sizeof p,"%s/app/actions",n);append(p,action);
 struct timespec t={0,250000000};nanosleep(&t,0);return 0;
}''')
        app=bindir/"fake-app"
        subprocess.run(["cc","-o",str(app),str(app_source)],check=True,capture_output=True)
        app.chmod(0o700)

        driver = live / "fake-driver"
        driver.write_text(r'''#!/usr/bin/python3
import json,os,pathlib,signal,subprocess,sys,time
control=pathlib.Path(os.environ['CONTROL']);rig=pathlib.Path(sys.argv[4])
run=subprocess.run([sys.argv[2],'hyprctl','-j','monitors'],text=True,capture_output=True,check=True)
assert json.loads(run.stdout)==[{'name':'fixture-monitor'}]
calls=control/'driver-calls';n=int(calls.read_text())+1 if calls.exists() else 1;calls.write_text(str(n))
mode=os.environ.get('HN_CHAIN_MODE','')
if mode=='scratch_mode' and n==1: rig.chmod(0o755)
if mode=='scratch_removed' and n==1:
 import shutil;shutil.rmtree(rig)
print('PASS controlled driver monitor IPC')
if mode=='functional_one_next' and n==1:sys.exit(1)
if mode=='repeat_term':
 owner=int(os.environ['HN_LIVE_INVOCATION_OWNER_PID'])
 os.kill(owner,signal.SIGTERM);os.kill(owner,signal.SIGTERM)
 time.sleep(.03)
''')
        driver.chmod(0o700)
        shutil.copy2(driver,live/"task6_driver_checks.py")

    @staticmethod
    def install_boundary_check(path, mode, receipt_mode):
        body = '''#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
hn_live_begin 1
'''
        if mode == "bypass_common_exit":
            body += 'trap - EXIT\nprintf "CHECK_EXIT_TRAP_BYPASSED\\n"\nexit 0\n'
        else:
            body += '''trap - EXIT
python3 "$HN_LIVE_DIR/lifecycle.py" finish "$HN_LIVE_SESSION_DIR" >/dev/null
python3 - "$HN_LIVE_SESSION_DIR" "$HN_LIVE_EVIDENCE_DIR" "$TMPDIR" "$HN_LIVE_INVOCATION_OWNER_PID" "$HN_CHAIN_RECEIPT_MODE" <<'PY'
import json,os,pathlib,shutil,subprocess,sys
session,evidence,tmp,owner,kind=sys.argv[1:]
receipt=pathlib.Path(session)/'receipt.json'
if kind=='missing': receipt.unlink()
elif kind=='invalid': receipt.write_text('{}')
elif kind=='cross_session':
 scratch=pathlib.Path(tmp)/'foreign-session-scratch';scratch.mkdir(mode=0o700)
 other=subprocess.run([sys.executable,os.path.join(os.environ['HN_CHAIN_LIVE_DIR'],'lifecycle.py'),'init',evidence,str(scratch),owner],text=True,capture_output=True,check=True).stdout.strip()
 subprocess.run([sys.executable,os.path.join(os.environ['HN_CHAIN_LIVE_DIR'],'lifecycle.py'),'finish',other],check=True,capture_output=True)
 shutil.copyfile(pathlib.Path(other)/'receipt.json',receipt)
PY
exit 0
'''
        path.write_text(body)
        path.chmod(0o700)

    @staticmethod
    def install_prestart_failure_check(path):
        path.write_text('''#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
hn_live_begin 1
echo PRESTART_CHECK_COMPLETED
''')
        path.chmod(0o700)

    @staticmethod
    def install_signal_check(path):
        path.write_text('''#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"
hn_live_begin 1
kill -TERM "$HN_LIVE_INVOCATION_OWNER_PID"
kill -TERM "$HN_LIVE_INVOCATION_OWNER_PID"
sleep .03
''')
        path.chmod(0o700)

    @staticmethod
    def collect_result(root, control, evidence, result):
        launches = []
        try: launches = [int(x) for x in (control / "launch-pids").read_text().splitlines()]
        except FileNotFoundError: pass
        (control / "allow-exit").touch()
        # Creator-owned fake compositor children exit on marker if controller did not already TERM them.
        unreaped=[]
        for pid in launches:
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                try:
                    got,_=os.waitpid(pid,os.WNOHANG)
                    if got==pid: break
                except ChildProcessError:
                    break
                time.sleep(.01)
            else: unreaped.append(pid)
        session_data=[]
        for session in sorted(evidence.glob("session-*")):
            state_path=session/"state.json"
            try: state=json.loads(state_path.read_text())
            except (OSError,ValueError): state={"invalid":True}
            receipt_path=session/"receipt.json"
            try: receipt=json.loads(receipt_path.read_text())
            except (OSError,ValueError): receipt=None
            session_data.append({"path":str(session),"state":state,"receipt":receipt,
                                 "receipt_exists":receipt_path.exists()})
        app_pids=[]
        try: app_pids=[int(x) for x in (control/"app-pids").read_text().splitlines()]
        except FileNotFoundError: pass
        app_exited=[]
        try: app_exited=[int(x) for x in (control/"app-exited-pids").read_text().splitlines()]
        except FileNotFoundError: pass
        fixture_events=""
        for name in ("host-commands","app-actions"):
            try: fixture_events+=(control/name).read_text()
            except FileNotFoundError: pass
        finish_errors=""
        for name in ("lifecycle-finish-stderr","lifecycle-verify-stderr"):
            try: finish_errors+=(control/name).read_text()
            except FileNotFoundError: pass
        record_errors=""
        try: record_errors=(control/"lifecycle-record-stderr").read_text()
        except FileNotFoundError: pass
        check_errors=""
        try: check_errors=(control/"check-error-trace").read_text()
        except FileNotFoundError: pass
        signal_count=0
        try: signal_count=len((control/"term-signals").read_text().splitlines())
        except FileNotFoundError: pass
        return {"status":result.returncode,"stdout":result.stdout,"stderr":result.stderr,
                "launches":launches,"term":(control/"received-term").exists(),"signal_count":signal_count,
                "compositor_self_exit":(control/"self-exited").exists(),
                "sessions":session_data,"finish_calls":len((control/"finish-calls").read_text().splitlines()) if (control/"finish-calls").exists() else 0,
                "driver_calls":int((control/"driver-calls").read_text()) if (control/"driver-calls").exists() else 0,
                "refusals":len((control/"refusal-attempts").read_text().splitlines()) if (control/"refusal-attempts").exists() else 0,
                "app_pids":app_pids,"app_exited":app_exited,"unreaped":unreaped,
                "fixture_events":fixture_events,"finish_errors":finish_errors,"record_errors":record_errors,
                "check_errors":check_errors}

    def assertNoChildrenRemain(self, control, summary):
        self.assertEqual(summary["unreaped"], [], "fixture compositor child not reaped by creator")
        deadline=time.monotonic()+3
        while time.monotonic()<deadline and len(summary["app_exited"])<len(summary["app_pids"]):
            try: summary["app_exited"]=[int(x) for x in (control/"app-exited-pids").read_text().splitlines()]
            except FileNotFoundError: pass
            time.sleep(.01)
        self.assertEqual(sorted(summary["app_exited"]),sorted(summary["app_pids"]),
                         "fixture-owned application did not exit through controller or creator marker")

    def test_actual_chain_scratch_mode_change_after_dispatch(self):
        r=self.exercise("scratch_mode")
        self.assertUnsafeOwnershipRetained(r,"mode-0700")

    def test_actual_chain_scratch_removed_after_dispatch(self):
        r=self.exercise("scratch_removed")
        self.assertUnsafeOwnershipRetained(r,"path component missing")

    def test_actual_chain_check_exit_one_before_started_flag(self):
        r=self.exercise("exit_one_before_started")
        self.assertEqual(r["status"],1,r["stdout"]+r["stderr"])
        self.assertEqual(len(r["launches"]),2)
        self.assertIn("PRESTART_CHECK_COMPLETED",r["stdout"],"next real check did not complete after clean first receipt")
        self.assertEqual(len(r["sessions"]),2)
        self.assertTrue(all(s["receipt"]["state"]=="CLEAN" for s in r["sessions"]))

    def test_actual_chain_semantic_guard_refusal(self):
        r=self.exercise("guard_refusal")
        self.assertEqual(r["status"],2,r["stdout"]+r["stderr"])
        self.assertGreater(r["refusals"],0)
        self.assertFalse(r["term"])
        self.assertEqual(r["signal_count"],0)
        self.assertEqual(len(r["launches"]),1)
        self.assertEqual(len(r["sessions"]),1)
        self.assertEqual(r["sessions"][0]["state"]["phase"],"ACTIVE")
        self.assertFalse(r["sessions"][0]["receipt_exists"])
        self.assertNotIn("RUN  float_fallback",r["stdout"])

    def test_actual_chain_check_bypasses_common_exit(self):
        r=self.exercise("bypass_common_exit")
        self.assertEqual(r["status"],0,r["stdout"]+r["stderr"])
        self.assertIn("CHECK_EXIT_TRAP_BYPASSED",r["stdout"])
        self.assertEqual(len(r["launches"]),3,"matrix failed to gate bypassed check and both float sessions")
        self.assertEqual(len(r["sessions"]),3)
        self.assertTrue(all(s["receipt"] and s["receipt"]["state"]=="CLEAN" for s in r["sessions"]))

    def test_actual_chain_repeated_term_finalizes_once(self):
        r=self.exercise("repeat_term")
        self.assertEqual(r["status"],2,r["stdout"]+r["stderr"])
        self.assertEqual(len(r["launches"]),1)
        self.assertEqual(r["finish_calls"],2,"repeat TERM reentered matrix finalization (expected one common + one matrix finish)")
        self.assertNotIn("RUN  float_fallback",r["stdout"])
        self.assertTrue(r["sessions"][0]["receipt_exists"] or r["sessions"][0]["state"]["phase"]!="CLEAN")

    def test_actual_chain_missing_invalid_cross_session_receipt(self):
        for receipt_mode in ("missing", "invalid", "cross_session"):
            with self.subTest(receipt_mode=receipt_mode):
                r=self.exercise("receipt", receipt_mode=receipt_mode)
                self.assertEqual(r["status"],2,r["stdout"]+r["stderr"])
                self.assertEqual(len(r["launches"]),1)
                self.assertNotIn("RUN  float_fallback",r["stdout"])
                self.assertIn("-j monitors sig=fixture-instance",r["fixture_events"],
                              "receipt gate was not reached through nested CLI")
                if receipt_mode=="missing":
                    self.assertFalse(r["sessions"][0]["receipt_exists"])
                elif receipt_mode=="invalid":
                    self.assertEqual(r["sessions"][0]["receipt"],{})
                else:
                    self.assertEqual(len(r["sessions"]),2)
                    # sessions are sorted by random ID; select the victim by its compositor record.
                    victims=[x for x in r["sessions"]
                             if any(k.get("kind")=="compositor" for k in x["state"].get("records",[]))]
                    self.assertEqual(len(victims),1)
                    self.assertNotEqual(victims[0]["receipt"].get("session"),
                                        victims[0]["state"].get("session"))

    def test_actual_chain_happy_clean_receipt_allows_next_check(self):
        r=self.exercise("happy_next")
        self.assertEqual(r["status"],0,r["stdout"]+r["stderr"])
        self.assertEqual((len(r["launches"]),r["driver_calls"],len(r["sessions"])),(2,2,2))
        self.assertTrue(all(s["receipt"] and s["receipt"]["state"]=="CLEAN" for s in r["sessions"]))

    def test_actual_chain_nested_window_moved_off_workspace_4_fails_check(self):
        r=self.exercise("window_moved")
        self.assertEqual(r["status"],1,r["stdout"]+r["stderr"])
        self.assertIn("not parked on host workspace 4",r["stdout"]+r["stderr"])
        self.assertEqual(len(r["sessions"]),1)
        self.assertEqual(r["sessions"][0]["receipt"]["state"],"CLEAN")

    def test_actual_chain_functional_one_clean_receipt_allows_next_check(self):
        r=self.exercise("functional_one_next")
        self.assertEqual(r["status"],1,r["stdout"]+r["stderr"])
        self.assertEqual((len(r["launches"]),r["driver_calls"],len(r["sessions"])),(2,2,2))
        self.assertIn("FAIL rules_applied",r["stdout"])
        self.assertGreaterEqual(r["stdout"].count("PASS rules_applied"),1)
        self.assertTrue(all(s["receipt"] and s["receipt"]["state"]=="CLEAN" for s in r["sessions"]))

    def test_actual_chain_float_unsafe_first_blocks_second_and_preserves_assertions(self):
        r=self.exercise("float_unsafe_first")
        self.assertEqual(r["status"],2,r["stdout"]+r["stderr"])
        self.assertEqual(len(r["launches"]),1)
        self.assertEqual(len(r["sessions"]),1)
        self.assertGreater(r["refusals"],0)
        self.assertIn("organizerFloating=False observed",r["stdout"])
        self.assertNotIn("organizerFloating=True observed",r["stdout"])
        self.assertNotIn("RUN  rules_applied",r["stdout"])

    def test_actual_chain_float_cases_keep_both_functional_assertions(self):
        r=self.exercise("float_both")
        self.assertEqual(r["status"],0,r["stdout"]+r["stderr"])
        self.assertEqual((len(r["launches"]),len(r["sessions"])),(2,2))
        self.assertIn("organizerFloating=False observed",r["stdout"])
        self.assertIn("organizerFloating=True observed",r["stdout"])
        self.assertEqual(len({s["state"]["scratch"] for s in r["sessions"]}),2)
        self.assertTrue(all(s["receipt"] and s["receipt"]["state"]=="CLEAN" for s in r["sessions"]))

    def assertUnsafeOwnershipRetained(self,r,error_text):
        self.assertEqual(r["status"],2,r["stdout"]+r["stderr"])
        self.assertEqual(len(r["launches"]),1)
        self.assertEqual(r["driver_calls"],1)
        self.assertFalse(r["term"],"strict scratch precondition should refuse signal path")
        self.assertEqual(r["signal_count"],0)
        self.assertEqual(len(r["sessions"]),1)
        self.assertNotEqual(r["sessions"][0]["state"]["phase"],"CLEAN")
        self.assertFalse(r["sessions"][0]["receipt_exists"],"uncertain scratch received clean receipt")
        self.assertTrue(r["sessions"][0]["state"]["records"],"durable owner record was lost")
        self.assertIn(error_text,r["finish_errors"])
        self.assertNotIn("RUN  float_fallback",r["stdout"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
