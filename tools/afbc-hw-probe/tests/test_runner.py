# SPDX-License-Identifier: MIT
# Exercise the shell runner without a GPU, real Mali library, or timeout utility.
from pathlib import Path
import tempfile,shutil,subprocess,os,json
SOURCE=Path(__file__).resolve().parents[1]/'run.sh'
root=Path(tempfile.mkdtemp(prefix='afbc-runner-'))
package=root/'package';package.mkdir();shutil.copy(SOURCE,package/'run.sh')
probe=package/'probe-armhf'
probe.write_text('''#!/bin/sh
case_name=self-test
while [ "$#" -gt 0 ]; do
 case "$1" in --case) case_name=$2;shift 2;; *) shift;; esac
done
echo "mock_only=1 case=$case_name"
if [ "$case_name" = "${AFBC_TEST_FAIL:-none}" ]; then echo 'RESULT=FAIL';exit "${AFBC_TEST_EXIT_CODE:-1}";fi
echo 'RESULT=PASS'
''');probe.chmod(0o700)
library=root/'libMali.so';library.write_bytes(b'non-driver-test-fixture')
fakebin=root/'bin';fakebin.mkdir()
# CoreELEC-like PATH: include ordinary shell utilities, deliberately no timeout.
for name in ['dirname','date','uname','cat','tr','ls','readlink','awk','strings','grep','tail','dmesg','tar','gzip','basename','mkdir','tee']:
 path=shutil.which(name)
 if path:(fakebin/name).symlink_to(path)
sha=fakebin/'sha256sum';sha.write_text('#!/bin/sh\necho "$AFBC_TEST_SHA  $1"\n');sha.chmod(0o700)
known='949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397'
stripped='789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445'
assert subprocess.run(['/bin/sh','-c','command -v timeout'],env={**os.environ,'PATH':str(fakebin)},stdout=subprocess.DEVNULL).returncode!=0
full=['self-test','info','local','linear','afbc','afbc-split']
tests=[
 ('all-original',[],None,1,known,full,0),
 ('all-stripped',[],None,1,stripped,full,0),
 ('split-only',['--profile','split'],None,1,stripped,full[:4]+['afbc-split'],0),
 ('plain-only',['--profile','plain'],None,1,known,full[:5],0),
 ('linear-fail',[],'linear',1,known,full[:4],1),
 ('afbc-fail',[],'afbc',1,known,full[:5],1),
 ('timeout',[],'info',124,known,full[:2],1),
 ('crash',[],'info',137,known,full[:2],1),
 ('self-test-fail',[],'self-test',9,known,full[:1],1),
 ('unknown-driver',[],None,1,'unrecognized',full[:3]+['private-imports'],0),
 ('info-only',['--info'],None,1,known,full[:2],0),
]
report=[]
for name,args,failure,fail_code,identity,expected,exit_code in tests:
 out=root/name;env={**os.environ,'PATH':str(fakebin),'AFBC_TEST_SHA':identity}
 if failure:env['AFBC_TEST_FAIL']=failure;env['AFBC_TEST_EXIT_CODE']=str(fail_code)
 r=subprocess.run(['/bin/sh',str(package/'run.sh'),*args,'--library',str(library),'--output',str(out)],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
 rows=[x.split('\t') for x in (out/'summary.tsv').read_text().splitlines()[1:]]
 assert [x[0] for x in rows]==expected,(name,rows,r.stdout)
 assert r.returncode==exit_code,(name,r.returncode,r.stdout)
 assert Path(str(out)+'.tar.gz').exists()
 if name=='timeout':assert rows[-1][1:] == ['TIMEOUT','124'],rows
 if name=='crash':assert rows[-1][1:] == ['CRASH','137'],rows
 if name=='self-test-fail':assert rows[-1][2]=='9',rows
 report.append(dict(name=name,passed=True,cases=[x[0] for x in rows],returncode=r.returncode,external_timeout_available=False))
print(json.dumps(report,indent=2))
