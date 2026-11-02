# SPDX-License-Identifier: MIT
from pathlib import Path
import os,subprocess,tempfile,time,signal,select,json,argparse
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--probe',type=Path,required=True)
BIN=parser.parse_args().probe.resolve()
TEST=Path(tempfile.mkdtemp(prefix='afbc-watchdog-'))
fifo=TEST/'blocked-library.so';os.mkfifo(fifo)
env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'}
results=[]
def run(args,expected,tag):
 start=time.monotonic()
 r=subprocess.run([str(BIN),*args],cwd=TEST,env=env,capture_output=True,text=True,timeout=8,start_new_session=True)
 elapsed=time.monotonic()-start
 assert r.returncode==expected,(tag,r.returncode,r.stdout,r.stderr)
 results.append(dict(test=tag,exit_code=r.returncode,seconds=round(elapsed,3)))
 return r
run(['--watchdog','3','--self-test'],0,'supervised-self-test')
r=run(['--watchdog','1','--case','info','--library',str(fifo)],124,'deadline-kills-blocked-child')
assert 'RESULT=TIMEOUT' in r.stderr
run(['--watchdog','3','--case','info','--library',str(TEST/'missing.so')],1,'child-failure-propagated')
run(['--watchdog','0','--self-test'],1,'invalid-deadline-rejected')
for sig,target,expected,tag in [(signal.SIGTERM,'parent',143,'parent-interrupt-cleans-child'),(signal.SIGKILL,'child',137,'child-crash-is-not-timeout')]:
 p=subprocess.Popen([str(BIN),'--watchdog','5','--case','info','--library',str(fifo)],cwd=TEST,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 assert select.select([p.stdout],[],[],4)[0], 'watchdog did not start'
 line=p.stdout.readline();assert 'watchdog_child_pid=' in line,line
 child=int(line.split('watchdog_child_pid=')[1])
 os.kill(p.pid if target=='parent' else child,sig)
 out,err=p.communicate(timeout=4)
 assert p.returncode==expected,(tag,p.returncode,line+out,err)
 try:os.kill(child,0)
 except ProcessLookupError:pass
 else:raise AssertionError('watchdog left its child behind')
 results.append(dict(test=tag,exit_code=p.returncode,child_reaped=True))

print(json.dumps(results,indent=2))
