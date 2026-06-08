# Codec Logos → Active Area — Design

**Date:** 2026-06-08
**Status:** Approved (design), pending implementation plan
**Scope:** xbmc (1 new infolabel) + skin.p3i.estuary + skin.plextuary (`skin.plextuarycpm`)

## Problem

On letterboxed/cropped Dolby Vision content the actual picture occupies only a
sub-rect of the frame; the rest is black bars (source L5, the pixel detector, or
the auto-letterbox synthesis). The codec logos in the OSD / PlayerProcessInfo are
top-anchored and therefore sit **in the top black bar**, floating off the picture
instead of over it.

We want an option to push the codec logos **down into the active area** so they
land on the picture, not the bar.

## Goal & non-goals

- **Goal:** when enabled, slide the codec-logo group down so it clears the DV top
  letterbox bar, correct at both 1080p and 2160p.
- **Non-goal:** moving logos for non-DV letterboxed content. The active area is
  only exposed for DV (`Player.Process(video.dovi.l5.*)`); plain aspect bars are
  not in scope.
- **Non-goal:** the `aml_dv_set_subtitles` "signal OSD to the DV core" path. That
  changes how the DV core treats the bars (and would re-expose grey on
  auto-letterbox/cropped content); it does **not** move the logos. Out of scope.

## Key constraint (drives the whole design)

Kodi does **not** evaluate `$INFO`/`$VAR` inside positional tags (`<left>`,
`<top>`, `<width>`, `<height>`) **or** inside an animation's `end=""` — they parse
as static floats. So the skin **cannot** read a continuous offset and position by
it. Positioning must be **bucketed**: a small set of discrete classes, each firing
a preset slide animation.

Because raw L5 offsets are in **coded pixels**, the same aspect ratio yields
different raw offsets at 1080p vs 2160p. 1080p DV is common, so bucketing on the
raw pixel value in XML would mis-classify half the library. The bucket boundary
must therefore be computed as a **fraction of frame height** — which requires
arithmetic, which only exists in C++. Hence the one C++ addition below.

## Architecture

### Component 1 — C++ active-area class infolabel (xbmc)

New Player.Process infolabel exposing a **resolution-independent** aspect class:

- **Token:** `video.dovi.active.area.class`
- **Constant:** `PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS`
- **Location:** computed in `xbmc/guilib/guiinfo/PlayerGUIInfo.cpp`, alongside the
  existing L5 block (`PLAYER_PROCESS_VIDEO_DOVI_L5_*`, ~line 708–772). Token mapped
  in `xbmc/GUIInfoManager.cpp` (next to the `video.dovi.l5.*` entries ~line
  1276–1282); define in `xbmc/guilib/guiinfo/GUIInfoLabels.h` (next to
  `PLAYER_PROCESS_VIDEO_DOVI_L5_*`, ~line 812–817).
- **Value:** integer string `0`/`1`/`2`/`3`.
- **Computation:** reuse the existing effective-offset precedence already in the
  L5 block (detected → auto-letterbox → source). Take the **effective top offset**
  and the **coded frame height**, compute `fraction = top / height`, and bucket:

  | Class | top-bar fraction | ~aspect | meaning |
  |-------|------------------|---------|---------|
  | 0 | < 0.04 | 16:9 / 1.85 | no meaningful bar |
  | 1 | 0.04 – 0.08 | ~2.0:1 | small bar |
  | 2 | 0.08 – 0.11 | ~2.2:1 | medium bar |
  | 3 | > 0.11 | ~2.40:1+ | large bar |

  When there is no L5 / no DV / `height == 0`, return `0`.

  Frame height source: the same metadata/data-cache used for the existing offset
  computation in that block (resolve during implementation; do not introduce a new
  data path if the height is already reachable there).

### Component 2 — Skin gating & slide (both skins, XML only)

**Trigger condition** (logos reposition when true):

```
[ !Skin.HasSetting(osd.codeclogosactiveareaoff)
  | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ]
+ !String.IsEqual(Player.Process(video.dovi.active.area.class),0)
```

- `System.GetBool(...)` reads the Kodi setting directly (verified: `SYSTEM_GET_BOOL`
  accepts an arbitrary boolean setting id). Restriction users get it automatically.
- `osd.codeclogosactiveareaoff` is the **opt-out** form of the new setting
  (Component 3). Kodi skin bools default off, so storing the opt-out makes the
  feature **default on**: absent setting = forced on; present = user opted out of
  forcing (restriction users still get the moved logos via the OR). The two
  triggers are independent.

**Slide presets** (1080 design-space; identical across resolutions because the GUI
bar fraction is resolution-independent — only the *class* needed correcting):

| Class | slide down (px, 1080-space) |
|-------|-----------------------------|
| 1 | +60 |
| 2 | +104 |
| 3 | +138 |

Implemented as per-class conditional `<animation effect="slide" end="0,<n>"
time="0" condition="...class,N...">Conditional</animation>` on the codec-logo
**group** (never wrap the `_Flag` includes in a new group — known gotcha: that
renders nothing). Add to the group the logos already live in.

**Render sites (differ per skin — confirmed against the actual XML):**

- **skin.plextuary** — logos appear in a *persistent* OSD and PPI, both via the
  shared `CodecLogos*OSD` includes (`xml/Includes_CodecLogos.xml`): the OSD
  references them in `Custom_1109_TopBarOverlay.xml`, the PPI in
  `DialogPlayerProcessInfo.xml`. Each include's **outer group** has `<left>35/855/1675</left>`
  and inner rows at `top50` (video) / `top152` (audio). Add the slide animations to
  that outer group → covers OSD **and** PPI in one place. **Skip** Plextuary's own
  startup flash (`VideoFullScreen.xml` `_Flag` invocations).
- **skin.p3i.estuary** — codec logos exist **only** as the startup flash (group
  `id=2` non-av1 / `id=21` av1 in `VideoFullScreen.xml`, gated on
  `Player.Process(av.change),1`); the p3i PPI is text-only. So the startup-flash
  group is the only surface — add the slide there. It already carries conditional
  position slides (L/C/R, horizontal); the new vertical slide composes with them
  (verify on-device that multiple active conditional slides sum). For native-DV
  source-L5 / auto-letterbox the class is known at stream-open so the flash slides
  correctly; the slower pixel-detector path flashes at top first (no regression).

**Axis:** top offset only. (Pillarbox DV ≈ nonexistent; logos are top-anchored.)

### Component 3 — New skin setting (both skins)

- **Stored id (opt-out):** `osd.codeclogosactiveareaoff` — present = user turned the
  force off; absent = forced on (the default). Mirrors p3i's existing
  `osd.hidecodecstartuplogos` opt-out pattern.
- **UI control:** a toggle labelled "Always keep codec logos inside the active area",
  shown *checked by default*: `<selected>!Skin.HasSetting(osd.codeclogosactiveareaoff)</selected>`,
  `onclick` → `Skin.ToggleSetting(osd.codeclogosactiveareaoff)`.
- **Semantics:** checked (default) = force the move for any DV letterboxed content;
  unchecked = stop forcing — the restriction-coupled path
  (`System.GetBool(...restrict.subs.active.area)`) still moves logos for restriction
  users. The two triggers are independent (OR).
- **Default behavior:** **on** (both skins).
- **Placement:** next to the existing codec-logo settings — Plextuary on the page
  with the logo position setting; p3i.estuary settings page 1 (after the preview toggle).
- New localized strings (en_gb + de_de), in each skin's string range.

## Data flow

1. DV playback → `PlayerGUIInfo` computes effective top offset (existing logic).
2. New infolabel divides by coded height → emits class `0–3`.
3. Skin trigger evaluates (`!opt-out` OR restriction setting) AND class≠0.
4. Matching per-class slide animation fires → logo group shifts down by the preset
   (Plextuary OSD+PPI via the shared include; p3i the startup-flash group).
5. Class is re-evaluated live as metadata updates (same cadence as the existing L5
   infolabels), so it follows source/detected/auto-letterbox changes.

## Edge cases

- **Non-DV / no L5 / height 0:** class `0` → no slide (logos stay at default top).
- **16:9 DV:** offsets 0 → class `0` → no slide.
- **Auto-letterbox cropped DV:** offsets come from the synthesis path → class
  reflects the synthesized bar → logos move. (No grey re-exposed; this path does
  not touch `aml_dv_set_subtitles`.)
- **Restriction off + opt-out set (force off):** never moves (current behavior
  preserved).
- **p3i pixel-detector path:** class is 0 during the brief startup flash (detector
  not yet stable) → flash shows at top, as today; no regression.
- **Mid-playback aspect change:** class updates live; slide re-evaluates.

## Testing

CE-only (DV active area is Amlogic-only; not Windows-testable). On-device A/B on
the AM6B+, **both skins**:
- 2160p DV 2.40:1, 2.0:1, 16:9 → class 3/1/0, logos clear the bar / sit correctly.
- **1080p DV** 2.40:1 and 2.0:1 → same classes/placement as 2160p (resolution
  correctness — the explicit reason for the C++ class).
- Default (no opt-out, restriction off) → logos move (default-on confirmed).
- Restriction setting on, opt-out set → logos still move (restriction OR path).
- Opt-out set, restriction off → logos stay at top (regression guard).
- **p3i specifically:** verify the new vertical slide composes with the existing
  L/C/R + horizontal position slides on group id=2 (no cancellation).
- Verify the codec logos still render (the wrap-in-group gotcha) and existing
  position/OSD/PPI behavior is unchanged when class 0.

## Risks / open items

- `Integer`/`String` comparison on a `Player.Process` infolabel: confirmed pattern
  exists (`Integer.IsGreater(ListItem.Playcount,...)`); class is a plain int string,
  compared via `String.IsEqual`. Low risk; verify on-device.
- Slide presets are eyeballed from the fraction table; fine-tune on-device if a
  class lands slightly into/over the bar.
- 1440p / unusual DV resolutions: handled correctly by the fraction-based class
  (no resolution branching needed in XML).
