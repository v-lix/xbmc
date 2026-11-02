#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run a native (optionally ASan/UBSan) probe on Mesa software GLES.

cc -O1 -g -std=c11 -Wall -Wextra -Werror -fPIC -pie \
  -fsanitize=address,undefined -fno-omit-frame-pointer fanart-probe.c \
  -o /tmp/fanart-host-asan -ldl -lm
python3 tests/test_probe.py --probe /tmp/fanart-host-asan

No Mali AFBC hardware is emulated by this test.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import select
import struct
import subprocess
import tempfile
import time
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', required=True, type=Path)
args = parser.parse_args()
probe = args.probe.resolve()
root = Path(tempfile.mkdtemp(prefix='afbc-fanart-test-'))
env = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
           ASAN_OPTIONS='detect_leaks=0')
report = []


def run(name, options, expected=0, timeout=120):
    out = root / name
    out.mkdir()
    start = time.monotonic()
    r = subprocess.run([str(probe), *options], cwd=out, env=env,
                       text=True, capture_output=True, timeout=timeout)
    (out / 'log.txt').write_text(r.stdout + r.stderr)
    assert r.returncode == expected, (name, r.returncode, r.stdout[-3000:], r.stderr)
    assert 'AddressSanitizer' not in r.stderr and 'runtime error:' not in r.stderr, (name, r.stderr)
    report.append(dict(test=name, exit_code=r.returncode,
                       seconds=round(time.monotonic()-start, 3)))
    return r, out


run('self-test', ['--self-test'])
for name, options in [
    ('width-zero', ['--width', '0']),
    ('width-too-large', ['--width', '4097']),
    ('bad-numeric', ['--frames', '1']),
    ('missing-photos', ['--pattern', 'photos']),
    ('bad-pattern', ['--pattern', 'other']),
    ('bad-profile', ['--profile', 'other']),
    ('bad-watchdog', ['--watchdog', '0']),
    ('pacing-too-long', ['--fps', '1', '--frames', '600']),
]:
    run(name, options, 1)
run('private-ABI-rejected-on-host', [], 2)
fifo = root / 'blocked-library.so'
os.mkfifo(fifo)
if not Path('/dev/ion').exists() and not Path('/dev/dma_heap/system').exists():
    # This must reach the CPU allocator failure without ever opening the FIFO
    # supplied as a Mali library, and without applying the private ABI gate.
    r, _ = run('memory-mode-no-Mali', ['--memory-check', '--library', str(fifo),
                                    '--watchdog', '3', '--width', '128', '--height', '128'], 1, 6)
    assert 'memory_only=1' in r.stdout and 'egl_loaded=0 mali_loaded=0' in r.stdout
    assert 'neither /dev/ion nor /dev/dma_heap/system available' in r.stderr
r, _ = run('watchdog', ['--local', '--library', str(fifo), '--watchdog', '1', '--fps', '0'], 124, 8)
assert 'RESULT=TIMEOUT' in r.stderr
for sig, target, expected, name in [
    (signal.SIGTERM, 'parent', 143, 'interrupt-reaps-child'),
    (signal.SIGKILL, 'child', 137, 'crash-is-not-timeout'),
]:
    p = subprocess.Popen([str(probe), '--local', '--library', str(fifo), '--watchdog', '5', '--fps', '0'],
                         cwd=root, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, start_new_session=True)
    assert select.select([p.stdout], [], [], 4)[0], 'watchdog did not start'
    line = p.stdout.readline()
    if 'watchdog_child_pid=' not in line:
        out, err = p.communicate(timeout=6)
        raise AssertionError((name, p.returncode, line+out, err))
    child = int(line.split('watchdog_child_pid=')[1])
    os.kill(p.pid if target == 'parent' else child, sig)
    out, err = p.communicate(timeout=4)
    assert p.returncode == expected, (name, p.returncode, line+out, err)
    try:
        os.kill(child, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError('child survived watchdog interruption')
    report.append(dict(test=name, exit_code=p.returncode, child_reaped=True))


def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name+data))


w, h = 17, 13
raw = b''.join(b'\x00' + bytes(v for x in range(w)
                             for v in ((x*17)%256, (y*19)%256, (x+y)*7%256, (x*13+y*7)%256))
               for y in range(h))
png = root / 'alpha input.png'
png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
jpeg = Path(__file__).with_name('fixture.jpg').resolve()
common = ['--local', '--library', 'libEGL.so.1', '--frames', '4', '--rounds', '1', '--fps', '0']
for name, options in [
    ('alpha-128', ['--width', '128', '--height', '128', '--pattern', 'alpha']),
    ('odd-lifecycle', ['--width', '129', '--height', '127', '--pattern', 'alpha', '--cycles', '3']),
    ('one-pixel', ['--width', '1', '--height', '1', '--pattern', 'gradient']),
    ('noise', ['--width', '127', '--height', '65', '--pattern', 'noise']),
    ('photos-resize', ['--width', '97', '--height', '63', '--pattern', 'photos',
                       '--image-a', str(png), '--image-b', str(jpeg)]),
    ('detail-4k', ['--width', '3840', '--height', '2160', '--pattern', 'detail']),
    ('noise-odd-4k', ['--width', '3839', '--height', '2159', '--pattern', 'noise']),
]:
    r, out = run(name, [*common, *options])
    assert 'RESULT=PASS test=fanart-crossfade' in r.stdout
    checks = [line for line in r.stdout.splitlines() if line.startswith('correctness=')]
    assert checks and all('mismatched_pixels=0' in line for line in checks), (name, checks)
    import csv
    rows = list(csv.DictReader((out/'frames.csv').open()))
    assert rows and {row['route'] for row in rows} == {'ordinary', 'candidate'}
    assert len(rows) == (3 if name == 'odd-lifecycle' else 1)*4*4
    report[-1].update(pixel_comparisons=len(checks), measured_frames=len(rows))

bad = root/'bad.png'
bad.write_bytes(b'not an image')
run('bad-photo-rejected', [*common, '--pattern', 'photos', '--image-a', str(bad), '--image-b', str(jpeg)], 1)
print(json.dumps(dict(scope='Host Mesa ordinary controls only; no AFBC GPU test',
                      log_directory=str(root), results=report), indent=2))
