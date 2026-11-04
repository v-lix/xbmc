#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production blend helper and compare its output using software GLES."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='background-blend-') as temporary:
    path = Path(temporary)
    (path / 'utils').mkdir()
    (path / 'system_gl.h').write_text('#pragma once\n#include <GLES2/gl2.h>\n#include <GLES2/gl2ext.h>\n')
    (path / 'utils/log.h').write_text('''#pragma once
constexpr int LOGINFO=1, LOGWARNING=2;
struct CLog { template<class... T> static void Log(int,const char*,T&&...){} };
''')
    command = ['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I'+str(path), '-I'+str(root / 'xbmc')]
    if os.environ.get('GLES_HEADERS'):
        command += ['-I'+os.environ['GLES_HEADERS']]
    command += [str(root / 'tools/background-blend/TestBlend.cpp'),
                '-Wl,--no-as-needed', '-l:libEGL.so.1', '-l:libGL.so.1', '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True,
                   env={**os.environ, 'EGL_PLATFORM':'surfaceless', 'LIBGL_ALWAYS_SOFTWARE':'1',
                        'ASAN_OPTIONS':'detect_leaks=0'})
