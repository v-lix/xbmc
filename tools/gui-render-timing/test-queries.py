#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the actual profiler against a deterministic recording GL fixture.

Requires C++17, EGL/GLES2 headers, ASan/UBSan; no GPU or driver is loaded.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mesa', action='store_true', help='also exercise real Mesa GLES elapsed queries')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
# Presentations are counted from the swap result, never from entering the stage:
# a frame that rendered nothing skips the swap and may sleep.
application = (root/'xbmc/application/Application.cpp').read_text()
result = application.index('m_lastRenderAttempt.present = renderSystem->GetPresentResult();')
presented = application.index('GUI_RENDER_TIMING::Presented(hasRendered &&', result)
assert presented - result < 120, 'Presented follows the present result'
condition = application[presented:application.index(';', presented)]
assert 'PresentResult::SWAP_ACCEPTED' in condition and 'PresentResult::UNREPORTED' in condition
assert application.count('GUI_RENDER_TIMING::Presented(') == 1
with tempfile.TemporaryDirectory(prefix='gui-render-timing-') as tmp:
    out = Path(tmp)
    (out/'utils').mkdir()
    (out/'system_gl.h').write_text('#pragma once\n#include <GLES2/gl2.h>\n#include <GLES2/gl2ext.h>\n')
    (out/'utils/log.h').write_text('''#pragma once
#include <sstream>
#include <string>
#include <vector>
constexpr int LOGINFO=1;
inline std::vector<std::string> messages;
// Each line keeps its format and arguments, so tests can pair labels and values.
struct Logged { std::string format; std::vector<std::string> args; };
inline std::vector<Logged> logged;
struct CLog {
  template<class T> static std::string Text(const T& value){ std::ostringstream text; text<<value; return text.str(); }
  template<class... T> static void Log(int,const char* message,T&&... args){
    messages.emplace_back(message); logged.push_back({message,{Text(args)...}}); }
};
''')
    command = ['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-DHAS_GLES=2', '-DHAS_LIBAMCODEC', '-DTARGET_LINUX', '-I'+str(out),
               '-I'+str(root/'xbmc'), '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(root/'tools/gui-render-timing/TestQueries.cpp'), '-o', str(out/'test')]
    # Optional Khronos headers for hosts without the development packages.
    if os.environ.get('GLES_HEADERS'):
        command.insert(1, '-I'+os.environ['GLES_HEADERS'])
    subprocess.run(command, check=True)
    subprocess.run([str(out/'test')], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0'})
    if args.mesa:
        command[command.index(str(root/'tools/gui-render-timing/TestQueries.cpp'))] = str(root/'tools/gui-render-timing/TestMesa.cpp')
        command += ['-Wl,--no-as-needed', '-l:libEGL.so.1', '-l:libGL.so.1']
        subprocess.run(command, check=True)
        subprocess.run([str(out/'test')], check=True,
                       env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0',
                            'EGL_PLATFORM':'surfaceless', 'LIBGL_ALWAYS_SOFTWARE':'1'})
print('PASS: disabled fast path; whole-render envelope without per-draw attribution; deferred reads; disjoint, foreign query, ring/segment limits; context retirement; debug toggle/resume; CPU-only fallback; layout part and release counts; per-stage scheduler samples; counted parts without clocks; crossfade paths; input-to-present latency; fade/decode/upload presentation tags; job baseline per capture, also against a concurrent job; DDR monitor parsing; every logged label paired with its value')
