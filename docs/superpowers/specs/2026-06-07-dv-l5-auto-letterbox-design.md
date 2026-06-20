# DV L5 Auto-Letterbox for Cropped Content — Design Spec

**Date:** 2026-06-07
**Branch:** `aml-4.9-21.3_dev`
**Status:** Design — awaiting on-device A/B validation before trusting default-ON
**Origin:** AVSForum report (post 64647979) — cropped DV content shows **grey** letterbox bars instead of black.

---

## Problem

Dolby Vision **Level 5 (L5)** metadata defines the *active picture area* — it tells the DV
display-management core which regions of the frame are letterbox/pillarbox so the core masks them
to **black** instead of applying picture processing to them.

"Cropped" DV content has the black bars **physically removed** from the encode. A 2.40:1 film is
stored as a `3840×1600` frame (not letterboxed into `3840×2160`). The correct, in-spec source L5 for
such content is `0,0,0,0` — the whole coded frame is active.

When that `1600`-tall frame is displayed on a `2160p` output, the **player/scaler pads it** with
`280px` bars top and bottom. DV trims can carry a **positive lift** (raised black level). The DV core
applies that lift across the *whole output frame including the player-added bars* → the bars come out
**grey** instead of black.

### Reference behavior (what "correct" is)

Per the report, **R9, the TV's internal player, and the Energy player** all handle this the same way:
they **ignore the source L5 for cropped content** and **synthesize L5 from the output geometry**
(`1600` in `2160` → `280/280`), so the player-added bars are masked black. They do this regardless of
whether the source L5 is correct (`0,0`), wrong (`280,280`), or missing.

CoreELEC today (and the T4 build) simply **forwards the source L5**. For correctly-authored cropped
content (`L5=0`) it forwards `0` → grey bars.

### Observed evidence that de-risks the fix

From the report, on **CE default** with a `1600px` cropped video carrying a source `L5=280,280`:
> "CE default sends 280/280, and letterbox is black without overcropping."

This is the key proof: **when the CE DV core is given `L5=280/280` on a cropped `1600px` frame, it
masks the player-added bars black with no overcrop.** The masking-space behavior is already correct.
The only bug is that we forward the source `0` instead of generating the `280` ourselves when the
source is correctly authored.

---

## Goal

Match R9: for cropped native-DV content, **synthesize the L5 active-area offsets from the coded
frame's geometry and override the source L5 unconditionally**, so the player-added letterbox/pillarbox
bars are masked black with positive lift preserved.

---

## Key insight: the value is a function of the coded dimensions alone

The offset R9 sends equals the gap needed to reach 16:9 *at the coded width*:

```
implied_16:9_height = coded_width * 9 / 16
top = bottom = (implied_16:9_height - coded_height) / 2      // letterbox (wide content)
```

For `3840×1600`: `3840*9/16 = 2160`; `(2160 − 1600)/2 = 280`. **Identical to R9's value.**

This number is **independent of the HDMI output mode** — it is purely a function of the coded frame
aspect. A `1920×800` cropped 2.40:1 file yields `(1080−800)/2 = 140`, and the core applies it
proportionally at whatever output resolution. **No render-thread geometry, no mode-switch timing, no
output-resolution lookup is required.**

The pillarbox (tall content, e.g. 4:3 in 16:9) case is symmetric:

```
implied_16:9_width = coded_height * 16 / 9
left = right = (implied_16:9_width - coded_width) / 2
```

The existing active-area detector **already computes exactly these two values** at
`xbmc/utils/AMLUtils.cpp:1991-1994` (`refH`, `refW`, `tbGap`, `lrGap`) and then **discards them**
(`DV_DETECT_SKIP_NON16X9`, `goto cleanup`). We are reclaiming a value the code already derives.

---

## Approach (chosen: A)

**Synthesize the geometry L5 at codec open and route it through the proven manual-override channel.**

The manual per-folder L5 override already exists end-to-end and is **confirmed by the report's data**
to (a) override a present source L5 (not merely fill when missing) and (b) produce black-no-overcrop:

- `DVDVideoCodecAmlogic::UpdateLevel5OverrideSettingCache()` parses the override and caches it.
- `DVDVideoCodecAmlogic::ApplyDynamicDoViSettings()` pushes it via
  `m_bitstream->SetLevel5Override(active, top, bottom, left, right)`
  (`xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp:210-223`).
- `CBitstreamConverter` substitutes the override into `DOVIFrameMetadata`
  (`xbmc/utils/BitstreamConverter.cpp:393-401`), which flows to the DV core.

We add an **auto** source for the same `SetLevel5Override` call: when the new setting is on, the
content is native DV, the frame is non-16:9, and **no manual override is set**, compute the geometry
offsets and feed them through the identical path.

This codec-open path is the **zero-I/O baseline**. The pixel-scan active-area detector is unified
with it (see "Unified detector" below) so that, when the scanner is enabled, it refines the geometry
baseline with any *additional* encoded bars it finds — without the two paths double-applying.

### Rejected alternatives

- **B — Render-thread compute → `xbmc_detected_l5_*` sysfs.** `detected_l5` is a "fill when missing"
  path that would not reliably override a present source `L5=0`; couples decode/render threads;
  more moving parts. Rejected.
- **C — Pure kernel-side** (core computes letterbox from input-vs-output res itself). Most
  self-contained / most R9-like, but most invasive, duplicates geometry the codec already has, and is
  hard to gate and observe for A/B. Deferred as possible future hardening.

---

## Detailed design

### Trigger / scope

- **Native DV only.** The grey-bars mechanism (positive-lift over player-added bars) is a DV-trim
  phenomenon; the existing detect path is likewise native-DV-gated. VS10/HDR10→DV conversions are out
  of scope.
- **Non-16:9 coded frame.** Reuse the detector's threshold: fire when `tbGap > 20 || lrGap > 20`
  (`AMLUtils.cpp:1995`). Below threshold → do nothing (true 16:9 content; encoded bars, if any, are the
  existing pixel-scan detector's job).
- **Letterbox vs pillarbox.** Wide frame → `top`/`bottom` set, `left`/`right` = 0. Tall frame →
  `left`/`right` set, `top`/`bottom` = 0. (Both zero is impossible past the threshold.)
- **Aspect source.** Use the coded dimensions (`m_hints.width` / `m_hints.height`) with the
  square-pixel formula above, matching the existing detector exactly. **Anamorphic edge case:** if
  `m_hints.aspect` is set and implies a display aspect materially different from `width/height`
  (SAR ≠ 1 — vanishingly rare for DV), derive the implied dimension from `m_hints.aspect` instead.
  Spec'd as a guarded refinement; square-pixel path is the default.

### Override semantics ("Match R9 exactly")

When the auto path is active, the synthesized value **replaces** the source L5 unconditionally —
source `0`, source `280`, or missing all collapse to the geometry value. This is inherent to routing
through `SetLevel5Override` (which forces the value).

**Precedence:** a manual per-folder L5 override (`SETTING_..._DV_LEVEL5_OVERRIDE`) **wins** — explicit
user intent beats automatic. The auto path only engages when the manual override is inactive.

### Setting

- **New setting:** `coreelec.amlogic.dolbyvision.l5.auto.letterbox` (final id TBD), boolean,
  **default ON**.
- Gated under the DV master + L5 settings group, alongside `...dolbyvision.level5` and
  `...dolbyvision.detect.active.area`, but **functionally decoupled** from `detect.active.area` — the
  auto path needs only the coded dimensions, no file I/O, so it works with the pixel scanner off.
- Strings added for `en_gb` and `de_de`.

### Where it runs

`DVDVideoCodecAmlogic`, in the decode thread, at codec open and on settings change — the same place
the manual override is computed and applied. The coded dimensions (`m_hints.width/height/aspect`) are
available there. Conceptually:

1. New cache fields mirroring the manual-override ones (`m_l5AutoLetterbox*`), refreshed in a
   `UpdateL5AutoLetterboxSettingCache()` and on `OnSettingChanged`.
2. In `ApplyDynamicDoViSettings()`, compute the **effective** L5 override:
   - if manual override active → use manual values (unchanged behavior);
   - else if auto setting on + native DV + non-16:9 **and the scanner is disabled** → compute geometry
     offsets, push via `SetLevel5Override(true, top, bottom, left, right)`;
   - else → `SetLevel5Override(false, …)` (no codec-open override; the scanner, if enabled, owns it —
     see "Ownership" below).
3. Native-DV gating: reuse the same DV-type signal the codec already tracks for other DV settings
   (`m_hints` profile / DV type cache used by the CMv4.0 append/strip paths).

### Unified detector ("smart" pixel scan)

Geometry and pixel-detection are the **same quantity in the same units** and they **stack**. The
report's data shows the core treats L5 relative to the intended 16:9 *canvas*: a `280` offset on a
`1600` coded frame masks the `280` player-added rows that live *outside* the coded frame. The coded
frame maps 1:1 into the middle of that canvas, so per side:

```
total_offset = geometry_gap            +  encoded_bar_scanned_inside_frame
               (player-added, outside)    (within the coded frame, coded px == canvas px)
```

One formula collapses all three regimes the detector faces:

| Content | geometry_gap | scanned inner bar | total |
|---------|--------------|-------------------|-------|
| 16:9 with encoded bars (today's normal scan) | 0 | 140 | 140 |
| pure cropped `1600px` (today: **skipped**) | 280 | 0 | 280 |
| cropped **and** extra encoded bars (hybrid, unhandled today) | 280 | 100 | **380** |

**Changes to `DetectActiveAreaFromFile` (`AMLUtils.cpp`):**

- The non-16:9 branch (`AMLUtils.cpp:1987-2003`) **no longer unconditionally skips**
  (`DV_DETECT_SKIP_NON16X9` / `goto cleanup` removed). Instead it seeds `geometry_gap`
  (`tbGap`/`lrGap`, already computed at `1991-1994`) as the **baseline**.
- **Early-skip (the common case, zero file seeking).** Before the expensive multi-position scan, do a
  cheap **single-frame inner-edge probe** on the first decoded frame: sample the top and bottom (or
  left/right) edge rows *of the coded frame*. If the content reaches the edges (bright → no inner
  bar), **emit `geometry_gap`, mark the detector stable, and stop** — no file-spanning seeks. This is
  the dominant cropped-content path and avoids the known I/O thrash on slow USB storage
  (cf. `dv-active-area-detect-io-thrash`).
- **Escalate only when warranted.** If the probe is inconclusive or suggests an inner bar (dark
  edge rows inside the coded frame), run the existing full multi-position scan to *measure* the inner
  bar, then emit `geometry_gap + scanned_bar`.
- For ordinary 16:9 content (`geometry_gap = 0`) the path is unchanged from today — the probe sees
  the encoded bars and the full scan measures them (`total = scanned_bar`).

The early-skip is the mechanism that makes "skip the detector early, if possible" concrete: *possible*
= a single-frame probe proves there is no inner bar, which is true for essentially all pure-cropped
content. Only genuine cropped+extra-encoded-bar hybrids pay the full scan.

**Probe safety (asymmetric, never wrong-but-cheap).** Bright edge rows *cannot* be a black bar, so
"bright → skip with geometry only" is always safe. "Dark → escalate" is also safe: a dark edge might
be an inner bar *or* just a dark scene / fade / title card, and that ambiguity is exactly what the
full multi-position scan with adaptive thresholds already resolves. The worst case of the early-skip
is an *unnecessary* full scan on dark-edged content — never an incorrect early result.

**Ownership (prevents double-application):** the codec-open auto path and the detector both inject L5,
through different kernel inputs. To guarantee exactly one writer:

- **Scanner enabled** (`detect.active.area` on): the detector is the **sole L5 owner** for cropped
  content and emits the stacked total. The codec-open geometry override is **suppressed** while the
  scanner is the active source.
- **Scanner disabled:** the codec-open geometry override owns it (geometry only, zero I/O) — this is
  the instant fix for users who keep the scanner off.
- **Manual per-folder override set:** wins over both.

Render-side consumers already prefer the detector when stable
(`RenderManager` / `aml_dv_detect_active_area_stable()`), so the suppression rule aligns with existing
precedence. Implementation must confirm the kernel does not simultaneously apply both an
`xbmc_override_l5_*` and an `xbmc_detected_l5_*` value (verify `build_level_5_data_select`
precedence) — the ownership rule keeps only one populated, but the kernel side should be checked.

### Interaction matrix

| Source L5 | Auto setting | Manual override | Result |
|-----------|--------------|-----------------|--------|
| `0,0` (correct, cropped) | ON | none | synth `280/280` → **black** (fixes the bug) |
| `280,280` (wrong, cropped) | ON | none | synth `280/280` → **black, no overcrop** |
| missing (cropped) | ON | none | synth `280/280` → **black**, lift preserved |
| any (cropped) | ON | set | manual override wins |
| any | OFF | none | source L5 forwarded (current behavior) |
| 16:9 content | ON | none | below threshold → no-op; pixel scanner unaffected |

The matrix shows the **scanner-disabled** ownership (codec-open path). With the **scanner enabled**,
the same cropped rows produce the same masking, but the value is the detector's stacked total
(`geometry_gap + inner_bar`) and the codec-open override stays inactive (one writer).

### Out of scope

- **Subtitle/OSD positioning** already handles cropped content via `displayLB` in
  `RenderManager::CalcOverlayActiveArea`. This change is purely about the **display masking the bars
  black** and does not touch the overlay path.
- The detector's **pixel-scanning core** (multi-position sampling, adaptive thresholds, stability
  logic) is reused as-is; only its non-16:9 *skip* is replaced with the seed-geometry + early-skip +
  stack flow above. Its 16:9 behavior is unchanged.

---

## Validation (gate before trusting default-ON)

On-device A/B with the thread's three sample files on a DV display with a positive-lift trim:

1. **Correct source (`L5=0`), cropped** → bars **black** (today: grey). Primary fix.
2. **Wrong source (`L5=280`), cropped** → bars **black, no overcrop** (already correct on CE;
   confirm the auto path doesn't regress it).
3. **Missing source L5, cropped** → bars **black**, **positive lift preserved**.

Run each of the three with the **scanner off** (codec-open geometry path) and **scanner on**
(detector path) — both must produce identical masking.

Also confirm:
- Normal **16:9 letterboxed** DV content is unaffected (no-op, encoded bars still handled).
- **Pillarbox** (4:3) DV content masks left/right black.
- **Cropped + extra encoded bar** hybrid (if a sample exists / can be synthesized): scanner-on masks
  the full `geometry_gap + inner_bar`; scanner-off masks only `geometry_gap` (documented limitation).
- **I/O:** pure-cropped content with the scanner **on** triggers the **early-skip** (no file-spanning
  seeks) — verify via the detector log line and absence of the cache-stall symptoms from
  `dv-active-area-detect-io-thrash`.
- Toggling the setting OFF restores forwarding of the source L5.

If any cropped case **overcrops** instead of masking, the core is applying L5 in coded-frame space in
a way the report's `280`-source case did not exercise — fall back to investigating Approach C
(kernel-side output-space masking). The report's data makes this unlikely.

---

## Risks / open items

- **Stacking-units assumption.** The `geometry_gap + inner_bar` sum assumes the core applies L5 in
  the intended-16:9-*canvas* space and that a coded-frame pixel equals a canvas pixel (1:1 middle
  mapping). The report's `280`-source-on-`1600`-frame data supports the canvas interpretation; the
  stacking of an *additional* inner bar on top is the unverified extrapolation. The pure-cropped and
  pure-16:9 cases (the common ones) don't depend on it; only the rare hybrid does. A/B the hybrid if a
  sample is available, otherwise ship and treat hybrid as best-effort.
- **Native-DV gate plumbing.** Confirm the codec exposes a reliable "this stream is native DV"
  signal at `ApplyDynamicDoViSettings()` time (the CMv4.0 append/strip settings already gate on DV
  type — reuse that).
- **Early-skip probe threshold.** The single-frame inner-edge probe needs a brightness threshold
  consistent with the full scan's adaptive logic. Keep it conservative (bias toward escalation) so a
  dark scene never causes a wrong early result; reuse the scan's sampling helper where possible.
- **Detector ownership vs codec-open path.** Verify the kernel L5 injection precedence
  (`build_level_5_data_select`) so an inactive codec-open override and an active detected value never
  both apply; the one-writer rule should make this moot but must be confirmed on-device.
- **Anamorphic DV.** SAR ≠ 1 is essentially nonexistent in DV; the guarded `m_hints.aspect` path
  covers it but is low-priority to verify.
- **Variable aspect mid-stream** (IMAX-style ratio changes). The coded resolution is fixed for the
  stream, so the synthesized L5 is constant — matching R9, which also keys off input resolution. Any
  in-frame ratio variation is the (untouched) pixel-scan detector's domain, not this path's.
