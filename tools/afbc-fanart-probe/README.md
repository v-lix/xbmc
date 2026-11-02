# G52 AFBC fanart conversion and crossfade probe

## Revision 2: cold-allocation diagnosis

Use a kernel containing linux-amlogic commit
`a0110f2b158631f98f31556de4a8e70902db6a61`. The earlier cache-flag fix alone did
not repair ARM64's device-less DMA sync: `get_dma_ops(NULL)` selected dummy
operations, so fresh cached zeros could survive the intended sync. The revised
kernel cleans/invalidates that alias and prints
`ion: ARM64 device-less page cache sync enabled`. This probe records that marker
from the available kernel log; absence can also mean that the log was unavailable
or the marker was evicted.

For a separate CPU-only allocation experiment, cold boot with Kodi AFBC disabled:

```sh
sh run.sh --memory-only --label cold-memory
```

This tests allocations corresponding to 128x128, 1080p and 4K, fills them with
0xa5, then applies 32 MiB of CPU cache pressure on the same CPU that requested
the allocation. It reports changes before/after pressure, including zeroed bytes.
It never loads EGL/Mali or imports/submits a GPU buffer. CPU affinity changes
apply only to the probe child. This mode does not require the driver hash gate.

**Use another cold boot for the normal GPU probe**: running the memory check
first warms the ION pools. Memory mode is deliberately never run automatically
before GPU cases. Report the first run, including failures; repeated success can
hide fresh-page problems. GPU-mode diagnostics now report CPU-sync operations
and guard damage offsets/values separately. EINVAL from CPU sync is an error;
only ENOTTY on explicitly uncached ION retains the documented legacy fallback.
The reported before/after hardware comparison now confirms the kernel repair:
all three old-build GPU runs failed, and one of three CPU-only runs lost
107,328 bytes to zeros after cache pressure without loading EGL/Mali. All six
updated-build GPU suites passed, totaling 1,872 pixel comparisons and 120 guard
checks with no damage. See `hardware-validation.json` for per-run results and
input archive hashes. A passing CPU-only run alone did not distinguish the builds.

These diagnostics remain in this standalone probe. Kodi's production fanart
path uses a single conversion draw without per-image readback or guard scans.

This standalone probe extends the initial 128x128 AFBC import test. **It does not enable AFBC inside Kodi.** It tests whether
converting retained fanart textures is correct and faster to sample during
crossfades on the installed CoreELEC Mali-G52 fbdev driver.

The package includes a prebuilt ARM32 hard-float executable. No compiler,
Python, external `timeout`, network access during testing, or replacement driver
is needed on CoreELEC. It neither stops Kodi nor changes any settings. There are
no framebuffer/OSD submissions, window surfaces, or calls to `eglSwapBuffers`.
The test renders offscreen; nothing from this probe should appear on the TV.

## Run on CoreELEC

Download and extract into a fresh directory:

```sh
cd /storage
wget -O afbc-fanart-probe.tar.gz https://raw.githubusercontent.com/v-lix/xbmc/test-probe/tools/afbc-fanart-probe/afbc-fanart-probe.tar.gz
tar -xzf afbc-fanart-probe.tar.gz
cd afbc-fanart-probe
sha256sum -c SHA256SUMS
sh run.sh --label idle
```

The defaults compare both AFBC profiles, including 1080p, 4K, incompressible
noise, alpha, and odd dimensions. Allow several minutes. Peak memory is roughly
250-400 MiB depending on the driver and input image dimensions. Each test case
has its own 180-second watchdog. If a case fails or times out, subsequent cases
are skipped and the collected logs are archived.

For the most relevant comparison, supply **two actual fanart JPEG/PNG files**:

```sh
sh run.sh --label idle --image-a /storage/pictures/fanart-a.jpg --image-b /storage/pictures/fanart-b.jpg
```

Use the same two files on the next runs. While playing the H.264 video that
normally exhibits the problem, with the GUI in a repeatable state, run:

```sh
sh run.sh --label h264 --image-a /storage/pictures/fanart-a.jpg --image-b /storage/pictures/fanart-b.jpg
```

Optionally repeat with HEVC and `--label hevc`. The label is a description you
provide; the probe does not start, identify, or control video playback. Keep
display mode, playback scene, GUI state and other settings consistent. Disabling
Kodi's crossfade suppression is appropriate for later Kodi comparisons; this
standalone test always performs its own fades independently of that setting.

The script prints the path to `afbc-fanart-results-*.tar.gz`. Send back the
archives from idle and H.264 runs. They contain timing CSVs, correctness results,
system information, file paths/hashes and kernel logs. **They do not contain your
source pictures or full decoded image dumps.** Without `--image-a/--image-b`,
only deterministic generated patterns are tested, and this is stated in the log.

Useful variations:

```sh
# Shorter sampling, still all correctness/dimension checks:
sh run.sh --label quick --quick
# Only the plain AFBC format:
sh run.sh --label plain --profile plain
# Repeat allocation/import/conversion/use/destruction in each process:
sh run.sh --label lifetime --profile plain --quick --cycles 3
# Unpaced completion-latency stress test, separate from the 60 FPS default:
sh run.sh --label unpaced --profile plain --fps 0
```

The runner also accepts `--frames 2..600`, `--rounds 1..4`, `--cycles 1..5`,
`--fps 0..120`, `--library PATH`, and `--output NEW_DIRECTORY`. The runner extends
the watchdog for longer paced configurations, up to 900 seconds per case, and
rejects configurations that cannot fit that limit. For direct executable use,
`--watchdog 1..900` sets the bound. The executable must be run from a writable
results directory. Use unpadded decimal integers for numeric runner options.

## What is compared

For each image the probe:

1. Decodes JPEG/PNG into RGBA, or generates deterministic pixels. Inputs are
   resized to the test dimensions with CPU bilinear interpolation if needed.
   JPEG EXIF orientation and ICC color management are not applied. This prepares
   identical source pixels for both routes; it is not a replacement Kodi decoder.
2. Uploads an ordinary texture using `glTexImage2D`, then verifies its pixels
   against the CPU source, including channel order and row orientation.
3. Allocates/imports an AFBC buffer and renders the uploaded texture into it
   **once**, with nearest filtering and blending disabled. The GPU produces the
   AFBC contents. Allocation/import, upload, and conversion times are reported
   separately. The original texture remains resident for the A/B comparison.
4. Samples the ordinary or AFBC pair into the **same ordinary RGBA output FBO**.
   It draws outgoing then incoming images with changing opacity and Kodi's
   `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
   GL_ONE_MINUS_DST_ALPHA, GL_ONE)` settings.
5. Compares every output pixel at five fade values with nearest and bilinear
   filtering, at identity coordinates and with a fractional crop/scale. Identity
   conversion must match exactly; blended/filtered results allow one channel
   value of rounding difference. Larger differences fail the case. Checksums
   and mismatch examples are logged.
6. Measures repeated crossfades with bilinear scaling, then checks allocation
   guards and releases all resources. `--cycles` repeats the complete lifecycle.

Ordinary controls do the same operation into an ordinary rendered-copy texture,
without any private import. This helps distinguish problems in the shader/test
from AFBC-specific issues, and exposes any ordinary upload-versus-rendered-copy
layout performance difference.

## Reading timings correctly

Default pacing targets 60 starts per second, without catching up in bursts after
a slow frame. Timed blocks alternate ordinary/candidate/candidate/ordinary, then
the reverse order, with four warm-up frames per block. Raw samples are saved to
each case's `frames.csv`. Statistics include mean, median, p95, p99, maximum and
counts above 16.667 ms.

- `cpu-submit`: CPU time issuing the draw work.
- `wall-completed`: time issuing and waiting for the frame's GPU work with
  `glFinish`. This includes scheduling/driver waits and possible competition
  from Kodi. It is **not a measured displayed frame interval or vsync miss**.
- `gpu-elapsed`: `GL_EXT_disjoint_timer_query` results, if supported. Disjoint
  or zero results are excluded and `valid_samples` reports the remaining count.

Readback, correctness checks, JPEG decoding, allocation, conversion, CSV writes,
and pacing sleeps are outside the repeated draw timing section. `glFinish`
serializes this experiment, which differs from Kodi's pipelined rendering.
Offscreen work adds GPU load while Kodi runs, so playback disturbance during the
test is not itself evidence for or against AFBC. Look for a repeatable A/B
difference, especially tail latency under the same video workload.

This test cannot measure actual DDR bandwidth, AFBC compression ratio, displayed
smoothness, final OSD scanout compatibility, VS10/HDR correctness or the cost of
integrating conversion into Kodi's shared upload context. A successful benchmark
is evidence for a small opt-in Kodi fanart integration, not proof that all
jerkiness is fixed. Reserved AFBC capacity and nonzero header counts are **not
RAM savings or compression-ratio measurements**.

## Driver and buffer contract

Only these exact 32-bit libraries can use the private import path:

```text
949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397  original
789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445  byte-verified stripped build
```

No binary patch is applied. The runner computes the installed library's SHA-256
and rejects unrecognized builds before GPU testing. The original import probe's
[ABI notes](../afbc-hw-probe/ABI.md) describe the private 68-byte pixmap object.
This probe preserves that object and the DMA-BUF until GLES and EGLImage users
are destroyed. It uses the same uncached legacy ION system heap (mask 1) that
passed the hardware test, with DMA-BUF CPU synchronization for initialization
and inspection. It never chooses a secure/framebuffer heap.

For the **two selected sparse 16x16 RGBA8 formats only**, the layout is:

```text
padded_width  = align(width, 16)
padded_height = align(height, 16)
header_bytes  = align((padded_width / 16) * (padded_height / 16) * 16, 1024)
body_bytes    = padded_width * padded_height * 4
allocation    = align(header_bytes + body_bytes, 4096) + 65536-byte tail guard
```

This matches the unmodified driver's external-wrapper header/body calculations
for both format words at 128x128, 129x129, 1920x1080, 3840x2160 and 3839x2159
(see `layout-validation.json`). At 4K it reserves 33,763,328 bytes per AFBC
allocation including the guard. It is not a general allocator for other AFBC
variants. Guard checks supplement pixel comparison; they do not establish
production suitability for all driver/hardware combinations.

## Build and local validation

```sh
sh build.sh --zig /path/to/zig-0.13.0
# Or use an ARM hard-float GNU compiler:
CC=arm-linux-gnueabihf-gcc sh build.sh
```

PIC/PIE is intentional: it avoids stdout/stderr COPY relocations incompatible
with this proprietary library. The source dynamically resolves EGL/GLES symbols
and has no build-time dependency on Mali or graphics headers. No proprietary
library is redistributed.

JPEG/PNG decoding uses unmodified `stb_image.h` from nothings/stb commit
`f0569113c93ad095470c54bf34a17b36646bbbb5` (SHA-256
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`). Its
dual public-domain/MIT license is included in the header.

Host-side validation instructions are in `tests/test_probe.py`. The local
software GLES tests exercise the ordinary control, decoding, conversion and
blending at small/odd dimensions and 4K; they **cannot test Mali AFBC hardware**.
The ARM self-test runs under QEMU and checks metadata size, layout calculations,
comparison failures, checksums and the driver allowlist. The hardware report
from this new probe remains the required next validation step.
