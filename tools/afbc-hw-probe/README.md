# CoreELEC G52 AFBC hardware probe

This package checks whether the existing Mali library can import an externally
allocated AFBC target, render into it, and sample its contents correctly.
It includes a prebuilt **32-bit ARM hard-float** executable for the CoreELEC
Amlogic-ng userspace. An aarch64 kernel is fine if its userspace supports ARM32.
No compiler, Python, external `timeout` command, or downloaded Mali library is
needed on the box. Once the package is downloaded, the test needs no network
access. The installed Mali library is used.

## Revision 2

The first CoreELEC hardware report stopped before any GPU test because the image
has no `timeout` command. Revision 2 supervises each test in a child process with
a native monotonic-clock watchdog. It kills only that test's process group at
the deadline, cleans up on interruption, and distinguishes a timeout from a
crashed child. It cannot repair a GPU/kernel hang that remains uninterruptible.

The report also identified the installed library as a stripped copy. Stripping
the pinned original with GNU ARM binutils 2.40 reproduced its exact SHA-256;
all 26 allocated ELF sections were identical. Both verified file hashes are now
recognized. This does not establish an AFBC hardware result: the first run never
reached the tests.

## Run on the box

Download the prebuilt package from the `test-probe` branch (no Kodi rebuild):

```sh
cd /storage
curl -fL https://raw.githubusercontent.com/v-lix/xbmc/test-probe/tools/afbc-hw-probe/afbc-hw-probe.tar.gz -o afbc-hw-probe.tar.gz
```

Or download `afbc-hw-probe.tar.gz` from this directory in your browser and copy it
to `/storage`. Then extract it and run:

```sh
cd /storage
tar -xzf afbc-hw-probe.tar.gz
cd afbc-hw-probe
sh run.sh
```

Run from SSH as the usual CoreELEC root user, with playback stopped. This is an
experimental private-driver import test: a driver failure could hang the GPU or
require a reboot. The probe does not stop/start Kodi, patch libraries, change
configuration, create an EGL window, swap display buffers, or submit OSD frames.
All rendering is off screen into small private allocations.

The script prints the path of a results archive, for example:

```
/storage/afbc-probe-results-20261102T120000Z-1234.tar.gz
```

Send back **that results archive**, including failed-test logs. It contains no
Mali binary or media files. It includes system/device information and the last
200 kernel-log lines before/after the run; review those logs if necessary before
sharing them.

For just capability collection and context creation:

```sh
sh run.sh --info
```

Other options:

```sh
sh run.sh --library /usr/lib/libMali.gondul.so
sh run.sh --profile split
sh run.sh --output /storage/my-afbc-test-results
```

The output directory must not already exist. `--profile plain` selects the first
AFBC format; `--profile split` selects the split-block format. Both still run the
ordinary GLES and linear-import controls first. The default is `both`.

Do not retry further GPU tests after a GPU fault/timeout without first resolving
or resetting the failed driver state. For a clean import rejection, the logs
identify the rejected profile; it does not prove every AFBC path is unsupported.

## What it tests

1. **self-test:** validates metadata offsets and the pixel comparison logic.
   Its intentionally damaged test pixel produces one expected `pixel_mismatch`
   line followed by `RESULT=PASS test=self-test`.
2. **info:** EGL/GLES versions/extensions, private registry exports, and ION heaps.
   Standard DMA-BUF modifier support is reported, not exercised by this probe.
3. **local:** ordinary GLES texture/FBO control; no external memory import.
4. **linear:** externally allocated linear ABGR8888 through the native-pixmap
   registry/EGLImage path. If this fails, AFBC tests are skipped.
5. **afbc:** AFBC ABGR8888 through the same path.
6. **afbc-split:** split-block AFBC ABGR8888 through the same path.

Each rendering test writes a 128x128 checker pattern with four colors and four
alpha values, then samples that texture into a separate ordinary RGBA target.
All 16,384 output pixels are checked, including alpha, twice with different
patterns. The first full clear initializes the image before incremental writes.

External targets use a private 1 MiB DMA-BUF, leaving ample room for the small
image's headers, body, and alignment. The driver computes its AFBC layout; the
probe does not pretend a linear pitch/height product is the compressed layout.
Memory starts zeroed with a 64 KiB end guard. The probe checks the guard and
records changed bytes in the initial AFBC header region. The first 16 KiB of the
allocation and the sampled RGBA output are saved for inspection. These bytes are
observations, not a standalone AFBC conformance decoder.

The allocator uses legacy ION, heap mask 1 (system), flags 0 (uncached), matching
the user's kernel API. If `/dev/ion` is absent, it tries `/dev/dma_heap/system`.
DMA-BUF CPU-access synchronization brackets initialization/inspection; unsupported
synchronization is tolerated only for the explicitly uncached ION allocation.
No framebuffer or secure heap is requested. `--heap-mask MASK` can select a
known appropriate alternative ION heap, but should be changed only after reading
the reported heap IDs. Bits 6/7, used for framebuffer/secure heaps here, are
rejected. An allocation failure is an allocator failure, not AFBC rejection.

## Exact-library restriction

The native registry object is a reverse-engineered private ABI, not the public
`fbdev_pixmap` structure. The wrapper enables these tests only for these two verified SHA-256 values:

```
949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397  original
789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445  CoreELEC stripped
```

Both are the same executable build from CoreELEC/opengl-meson revision
`7bddce621a0c1e0cc12cfc8b707e93eb37fc0f82`,
`lib/eabihf/gondul/r12p0/fbdev/libMali.so`.
It is not patched. The probe uses exported symbols via `dlsym`; it does not jump
to hardcoded offsets. For any other file hash, capabilities/local rendering are collected and
private-import tests are explicitly skipped. The checks are not bypassed based
only on a matching build string. `driver-identity-validation.json` records the
stripping comparison.

## Interpreting results

- `local=FAIL`: fix the EGL/context/shader environment first.
- `linear=FAIL`: the native import ABI, allocation, synchronization, or linear
  renderability is unresolved. No AFBC conclusion yet.
- `image_import=PASS`: an EGLImage was created; rendering has not yet been proved.
- `renderable_image=PASS`: its GLES framebuffer is complete.
- `RESULT=PASS test=afbc` or `afbc-split`: the selected import/render/sample test
  passed its pixel, alpha, header-change, and guard checks.
- `SKIP_UNRECOGNIZED_LIBRARY`: the private ABI was not attempted.
- `TIMEOUT`: the native watchdog exceeded its 60-second limit and requested
  termination of the child process group; keep the kernel logs.
- `CRASH`: the child exited from a signal; this is distinct from a watchdog
  timeout. A driver stuck in an uninterruptible kernel wait may require a reboot.

A successful AFBC result establishes a useful offscreen GPU path. It does not
prove Amlogic OSD compatibility, scanout fences, bandwidth savings, or elimination
of tearing. The next stage would submit these buffers through the kernel AFBC
OSD path with matching metadata and buffer lifetime management.

## Build and validation

Source is self-contained C11; no EGL/GLES development headers are required.

```sh
# In a CoreELEC build environment, using its ARM hard-float cross compiler:
CC=arm-linux-gnueabihf-gcc sh build.sh

# Or Zig 0.13.0, targeting an older glibc ABI:
sh build.sh --zig /path/to/zig
```

The included binary was cross-built with Zig 0.13.0 for ARMv7 hard-float, glibc
2.17 target. `-fPIC -pie` avoids COPY relocations for libc data symbols in that
cross-toolchain. It uses `/lib/ld-linux-armhf.so.3` and requires only the normal
C runtime/dlopen libraries, then loads Mali dynamically.

Validation completed before packaging:

- Warning-clean ARM build; shipped ARM executable self-test and failure paths
  executed under QEMU with an ARM glibc runtime.
- Host AddressSanitizer/UndefinedBehaviorSanitizer self-test.
- Actual offscreen GLES control on Mesa llvmpipe under sanitizers: both full-image
  comparisons passed. This validates the rendering/checking code, not Mali AFBC.
- ARM-generated registry metadata passed through the original Mali dispatch code
  in Unicorn: expected FD, pitch, offset, type and AFBC format reached the wrapper.
  Registry lookup was mocked and execution stopped before real import.
- Shell runner success, control-failure, both verified library hashes,
  unknown-library, timeout/crash classification, and info-only paths tested with
  a mock executable and a PATH containing no `timeout` command.
- Native watchdog tested against a blocked FIFO library open, a failed child,
  parent interruption, and a killed child; deadlines and child reaping verified.
  These are process supervision tests, not AFBC hardware tests.

**Mali hardware import/render tests have not been run here.** Your results are
needed to establish those steps. See `ABI.md` and `validation.json` for details.

To reproduce the new regression checks on a Linux development machine with
Python 3 and a C compiler (not required on CoreELEC):

```sh
cc -std=c11 -fPIC -pie -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined probe.c -ldl -o probe-host-asan
python3 tests/test_watchdog.py --probe ./probe-host-asan
python3 tests/test_runner.py
```
