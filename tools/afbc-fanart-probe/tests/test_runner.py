#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise CoreELEC shell orchestration, skip/failure handling, and quoting."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1]
root = Path(tempfile.mkdtemp(prefix='afbc-fanart-runner-'))
package = root/'package'
package.mkdir()
shutil.copy(source/'run.sh', package/'run.sh')
probe = package/'fanart-probe-armhf'
probe.write_text('''#!/bin/sh
name=$(basename "$PWD")
case "$*" in *--self-test*) name=self-test;; esac
echo "mock_only=1 case=$name"
if [ "$name" = "${AFBC_TEST_FAIL:-none}" ]; then echo 'RESULT=FAIL'; exit "${AFBC_TEST_RC:-1}"; fi
echo 'RESULT=PASS'
''')
probe.chmod(0o700)
library = root/'mock library.so'
library.write_bytes(b'not a real driver')
fakebin = root/'bin'
fakebin.mkdir()
for name in ['dirname', 'date', 'uname', 'cat', 'head', 'readlink', 'awk', 'tail',
             'dmesg', 'tar', 'gzip', 'basename', 'mkdir', 'grep']:
    path = shutil.which(name)
    if path:
        (fakebin/name).symlink_to(path)
sha = fakebin/'sha256sum'
sha.write_text('#!/bin/sh\necho "$AFBC_TEST_SHA  $1"\n')
sha.chmod(0o700)
known = '789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445'
plain = ['afbc-128-alpha', 'afbc-1080-detail', 'afbc-4k-detail', 'afbc-4k-noise', 'afbc-odd-detail']
split = [v.replace('afbc-', 'afbc-split-', 1) for v in plain]
controls = ['self-test', 'local-128-alpha', 'local-4k-detail']
photo_a = root/'a space.jpg'
photo_b = root/'b $(literal).png'
photo_a.write_bytes(b'fixture')
photo_b.write_bytes(b'fixture')
report = []
for name, extra, failure, rc, sha_value, expected, code in [
    ('all', [], None, 1, known, controls+plain+split, 0),
    ('memory-only', ['--memory-only'], None, 1, 'unknown',
     ['self-test', 'memory-128', 'memory-1080', 'memory-4k'], 0),
    ('memory-failure', ['--memory-only'], 'memory-128', 1, 'unknown',
     ['self-test', 'memory-128'], 1),
    ('plain', ['--profile', 'plain'], None, 1, known, controls+plain, 0),
    ('split', ['--profile', 'split'], None, 1, known, controls+split, 0),
    ('photo-paths', ['--profile', 'plain', '--image-a', str(photo_a), '--image-b', str(photo_b)],
     None, 1, known, controls+plain+['afbc-4k-photos'], 0),
    ('control-failure', [], 'local-128-alpha', 1, known, controls[:2], 1),
    ('AFBC-failure', [], 'afbc-4k-detail', 1, known, controls+plain[:3], 1),
    ('timeout', [], 'afbc-128-alpha', 124, known, controls+plain[:1], 1),
    ('crash', [], 'afbc-128-alpha', 137, known, controls+plain[:1], 1),
    ('unknown-library', [], None, 1, 'unknown', ['self-test', 'private-imports'], 2),
    ('self-test-failure', [], 'self-test', 9, known, ['self-test'], 1),
]:
    out = root/name
    env = dict(os.environ, PATH=str(fakebin), AFBC_TEST_SHA=sha_value)
    if failure:
        env.update(AFBC_TEST_FAIL=failure, AFBC_TEST_RC=str(rc))
    r = subprocess.run(['/bin/sh', str(package/'run.sh'), '--quick', '--library', str(library),
                        '--output', str(out), *extra], env=env,
                       text=True, capture_output=True, timeout=15)
    assert r.returncode == code, (name, r.returncode, r.stdout, r.stderr)
    rows = [line.split('\t') for line in (out/'summary.tsv').read_text().splitlines()[1:]]
    assert [row[0] for row in rows] == expected, (name, rows)
    if name == 'timeout':
        assert rows[-1][1:] == ['TIMEOUT', '124']
    if name == 'crash':
        assert rows[-1][1:] == ['CRASH', '137']
    assert Path(str(out)+'.tar.gz').exists()
    report.append(dict(test=name, passed=True, case_count=len(rows)))
for extra in [['--label', '../bad'], ['--frames', '0'], ['--fps', '-1'], ['--rounds', '9999999999'],
              ['--profile', 'bad'], ['--image-a', str(photo_a)], ['--fps', '1', '--frames', '600'], ['--frames', '08']]:
    r = subprocess.run(['/bin/sh', str(package/'run.sh'), *extra],
                       env=env, text=True, capture_output=True, timeout=5)
    assert r.returncode == 1, (extra, r.stdout, r.stderr)
    report.append(dict(test='reject '+repr(extra), passed=True))
print(json.dumps(report, indent=2))
