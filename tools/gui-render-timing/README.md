# Opt-in GUI CPU/GPU stage timing on Amlogic GLES

Turn on **Settings > System > Logging > Enable debug logging**. GUI timing
starts on the next application iteration and stops when debug logging is turned
off. No advanced-setting element or Kodi restart is required. Debug logging
selected through the command line or log level also enables the observer.

Browse the problem view for 20-30 seconds, including both scrolling with new
fanart and repeated fades between already-loaded images. Collect `kodi.log`.
Keep AFBC, asynchronous upload, motion-delay settings and resolution unchanged
between comparisons. Turn debug logging off after capture. Outstanding GPU
queries remain reserved until ready or context teardown, but their data is
ignored after stopping, so toggling debug on again starts a fresh capture.

The initial `GUI timing: enabled` line reports whether GPU elapsed queries
were loaded and their counter width. Roughly once a second two lines follow.
Results arrive later than their frame: `frame` identifies the original loop.

`GUI timing:` summarizes the interval:

- `frames` / `presents`: measured loop iterations and those whose swap was
  accepted. A frame that rendered nothing skips the swap, and may then sleep
  40 ms, so it is not a presentation.
- `late`: swaps returning more than 1.5 budgets after the previous swap,
  counted only between consecutive swapping frames, so idle periods and
  skipped swaps never count. This is the closest available proxy for a
  displayed hitch; it is still a CPU-side timestamp, not a scanout measurement.
- `late_moving`: late presentations while a container scrolled or a control
  animated in that or the previous frame.
- `late_after_input_ms[...]`: late presentations by time since the last input
  action: under 100, 300 and 600 ms, later, or with no input yet. With the
  default 500 ms ease-out scroll, the first bucket is the fastest motion.
- `cpu_over` / `gpu_over`: frames whose CPU work / valid GPU envelope exceeded
  the budget. These are work-budget observations, not missed scanouts.
- `gpu_invalid`: frames whose elapsed query returned an impossible value
  (above one second, such as a wrapped negative interval). Their GPU time is
  discarded and never selects a worst frame.
- `gpu_worst_ms` / `gpu_worst_frame`: the slowest valid GPU envelope.
- `dropped`, `disjoint`, `aborted`: skipped captures, GPU-clock resets and
  context teardowns.
- `crossfade[...]`: background fades in the back-to-front pass, one per
  fading image control per frame: `drawn` where the single-pass draw is
  submitted, `clipped` when the single-pass path had nothing visible to
  draw, or drawn separately because of a `stack` of more than one outgoing image, an
  image of the pair that is `not_ready`, a pair that is `ineligible` (it
  cannot share one quad, an image is opaque, or the shader is unavailable),
  or `disabled` with `singlepasscrossfade` off. The front-to-back pass draws
  no translucent image and is not counted.
- `input_to_present_ms[...]`: time from an input action to the next accepted
  swap: under 20, 34, 50 and 100 ms, or later. The earliest input waiting for
  a swap is measured, so repeated keys before one swap count once. Unlike
  `late`, this also sees a slow first frame after an idle period. `stale`
  counts inputs whose next swap came more than a second later, such as a key
  that changed nothing; they are left out of the buckets and the maximum.
  `input_to_present_max_ms` is the slowest answered input. This is the swap
  of the frame that handled the input; a scroll starting from rest moves on
  the following frame.
- `presents_with[fade decode upload]` / `late_with[fade decode upload none]`:
  accepted and late swaps of frames that actually drew a background blend
  (two images of a fading control submitted, separately or as a single-pass
  pair; a fade that was only visited, clipped or still waiting for its
  incoming image does not count), or during which a large-image decode or an
  asynchronous upload ran on a job worker at any time. A frame can carry
  several tags; `none` is a late swap with none of them. The tags show
  overlap, not cause: decode covers every large image (`CImageLoader`:
  posters and other large art as well as fanart), and upload covers only
  asynchronous uploads, not ones on the main thread.
- `jobs[decode=N/ms upload=N/ms]`: finished decodes and uploads and their
  summed wall time on the workers in the interval. Each capture starts from
  zero; a job running when it starts counts from that moment.
- `ddr[...]`: average and peak total DDR bandwidth in KB/s since the previous
  line, then per DMC channel `chN=avg/max/at_total_max`, from the Amlogic
  monitor. The GUI never enables it; `[off]` until it is enabled, for example
  from SSH, with Mali on channel 0 and the CPUs on channel 1:

      echo 1 > /sys/class/aml_ddr/mode
      echo 0:1 > /sys/class/aml_ddr/port   # MALI
      echo 1:0 > /sys/class/aml_ddr/port   # ARM
      cat /sys/class/aml_ddr/port          # check the mapping
      echo 0 > /sys/class/aml_ddr/mode     # afterwards

  Each line clears the statistics. Do not use `busy`: in its autodetect mode
  the driver ignores Mali traffic. Averages over a second can hide short
  bursts; the peak and the late tags above are the closer signals.
  All counters reset after each line.

`GUI timing worst CPU:` describes the frame with the most CPU work, chosen by
CPU work only:

- `start`: local wall-clock time the frame began, comparable with the
  `HandleKey` lines. `since_input_ms`: time from the last input action to the
  frame's start, 0 when the frame itself handled input, or `none`. `moving`: a container scrolled or a control animated in this frame.
- `budget_ms`: 1000 / GUI refresh rate, about 16.667 at 60 Hz.
- `cpu_work_ms`: summed main-thread wall time in the named stages, excluding
  presentation and idle time. It includes waits inside driver calls and locks.
- `thread_ms`: main-thread CPU time for the whole iteration. `loop_ms`: its
  wall time, including presentation.
- `fade_visits` / `fade_blends` / `fade_controls` / `max_fade_layers`:
  background-fade render visits, visits that drew a blend of two images,
  distinct fading controls and the largest stack. One control is
  visited in both the front-to-back and the back-to-front pass, so two visits
  can be one control.
- `entries[...]`: how often each control-processing part was entered.
  `items`, `info` and `load` are only counted, without clock reads: they run
  hundreds of times a frame, and the 32-bit Kodi on this 4.9 arm64 kernel has
  no compat vDSO, so every clock read is a system call. Their time stays in
  `controls`. Timed parts (`text`, `release`, `layouts`) also count returns
  to them from a nested part.
- `layout_items_freed`: list items whose layouts a container destroyed
  outside its kept range, counted where they are freed.
  `textures_released`: releases of textures that were allocated.
- `cpu_ms[...]`: wall time per stage. `thread_cpu_ms[...]`: main-thread CPU
  time per stage. Wall time well above thread CPU time in a stage means the
  thread waited there: on a lock, I/O or the scheduler.
- `sched_valid`, `core`, `migrations`, `sched_ms[...]`: scheduler counters
  of the main thread, sampled whenever it enters a different top-level stage,
  so each top-level stage includes the parts nested in it (`controls`
  includes `text`, `release` and `layouts`; `gui` excludes `fade`). For each stage with
  time: `wall/cpu/runq/voluntary/involuntary`. `cpu` is time on a CPU and
  `runq` time runnable but waiting for one, both from
  `/proc/thread-self/schedstat` (`sched_valid=false` when the kernel lacks
  `CONFIG_SCHED_INFO`; then both read 0). The remainder,
  `wall - cpu - runq`, is time asleep: a lock, I/O, a driver wait or vertical
  refresh. `voluntary` counts sleeps, `involuntary` preemptions. `core` is the
  CPU at the frame's start and end; `migrations` counts core changes between
  samples. Each sample reads schedstat, `getrusage(RUSAGE_THREAD)` and
  `sched_getcpu`, roughly 15-20 times a frame while debug logging is on.

The CPU array uses **exclusive** stages, so `gui` excludes `fade`. The observer
no longer attributes individual GPU draw stages:

| Stage | Measured work |
| --- | --- |
| `process` | Application input/messages and other `Process()` work (CPU only) |
| `move_other` | Remaining FrameMove work outside the following named scopes |
| `input` | Port events and InputManager processing |
| `controls` | WindowManager Process outside the timed parts below: control processing, animation, layout, item layouts, visibility conditions and texture allocation |
| `text` | Text layout of labels whose text changed; unchanged text is not laid out |
| `release` | Texture release |
| `layouts` | Container freeing item layouts outside its kept range, excluding nested texture releases |
| `window_move` | WindowManager FrameMove |
| `external` | Opening the external-call window, intentional sleep and re-lock wait |
| `setup` | Render entry, resource drains and render-state preparation |
| `video` | Window manager's `RenderEx()` calls; hardware video decoding is separate |
| `gui` | GUI draw work outside the separately measured background fades |
| `fade` | Background image control drawing its fading stack and incoming image |
| `after` | Post-render window actions, including closing windows |
| `composite` | Begin/end GUI composite work, when that route is active |
| `end` | Render finalization and bookkeeping before presentation |
| `present` | CPU wall time in `Flip()`, including pacing and EGL swap waits |

A long `present` alone does not prove a GPU bottleneck: a healthy renderer can
wait for the next vertical refresh. CPU `controls`, `input` or `external` spikes
localize the FrameMove overruns seen in the first hardware capture. Compare
wall work with thread CPU: long wall time with little thread CPU suggests waits
or descheduling. A total can exceed the budget when no individual stage does.

The first observer tried to split GPU timings around GUI/fade stages. The
supplied G52 log repeatedly reported the same interval for several stages.
Arm documents that draws within a tile/render pass complete together, so
per-draw attribution is not reliable:
https://community.arm.com/forums/f/mobile-graphics-and-gaming-forum/6869/gl_ext_disjoint_timer_query-for-performance/21538

The corrected observer uses one `GL_EXT_disjoint_timer_query` elapsed envelope
across rendering, ending before presentation. CPU stage changes and fade scopes
do not split it. Multiple render invocations in a modal iteration can produce
several envelopes; this remains a driver estimate, not a physical scanout timer.
Do not compare the old summed GPU-stage field directly to `gpu_render_ms`.
The fixed ring holds 24 frames with at most 32 render intervals each. Pending
queries are never reused. Results are read only after every interval reports
available. There is no added `glFinish`, `glFlush`, fence wait or blocking fetch.
Context destruction retires query names; unavailable binds/other contexts never
cause deletion against a different context. No queries run in upload workers.

The control-processing parts are measured only outside rendering, so they do
not split the GPU envelope. Each timed boundary reads the wall and thread
clocks, two system calls in the 32-bit process, while debug logging is on; a
counted part only increments a thread-local counter. Earlier captures timed
every item, visibility update and texture allocation, roughly 1,500
boundaries on a scroll step, about 3,000 system calls: possibly milliseconds
per frame of observer overhead (not measured on the device). With
debug logging off, a boundary is only a thread-local check. Debug logging
also writes key and CEC lines; keep it on for comparisons alike.

The observer adds query/clock/log overhead, and timer boundaries can alter Mali
batching. Treat it as a temporary diagnostic and compare with it disabled.
It does not measure OSD underflow, actual scanout deadlines, hardware video
engine time, or individual upload-worker GPU operations or per-control GPU costs. It does not change
crossfade frequency or presentation cadence.

`python3 tools/gui-render-timing/test-queries.py` compiles the production query
manager with a recording GL fixture under ASan/UBSan. Set `GLES_HEADERS` to a
Khronos include directory if EGL/GLES development headers are not installed.
Add `--mesa` to also exercise real Mesa GLES elapsed-query scopes and deferred
resolution on a host with libEGL/libGL installed. The Mesa test explicitly waits
for its own test work; the production observer does not. Hardware measurements
still require the device build/run.
