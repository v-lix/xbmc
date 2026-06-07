# Dynamic CMv4.0 → CMv2.9 Down-Convert for Old DV TVs

**Date:** 2026-06-07
**Branch:** aml-4.9-21.3_dev
**Status:** Design approved, not yet implemented

## Problem

A user with an old, CMv2.9-only Dolby Vision TV gets a **completely black screen with no OSD** when
playing CMv4.0 content. The TV does not fall back correctly: instead of reading only the CMv2.9 base
layer of the RPU and ignoring the CMv4.0 extension blocks, its DV decoder fails to lock the tunnel
entirely, blacking out the whole HDMI signal (OSD included, since the box composites into the DV
tunnel).

The fix is to give the user an opt-in setting that **strips the CMv4.0 extension metadata from each
RPU before it is forwarded**, leaving a clean CMv2.9 RPU the old TV can lock onto. This is the exact
structural inverse of the existing CMv4.0 *append* feature.

## What already exists (the reusable spine)

In `xbmc/utils/BitstreamConverter.cpp`, `ProcessDoViRpu()`:

- Every RPU frame is already parsed (`dovi_parse_unspec62_nalu`), optionally mutated, and
  re-serialized (`dovi_write_unspec62_nalu`), with a per-frame input/output NAL cache.
- CMv4.0 presence is detectable via `vdrDmData->dm_data.level254` (the existing `AppendCMv40()`
  gates on exactly this — `hasLevel254`).
- The forward path `AppendCMv40()` → `dovi_rpu_add_cmv40_safe_default_metadata(opaque)` adds the
  CMv4.0 blocks. The strip is its mirror image.
- Setting plumbing pattern is established: `DOVICMv40Mode` enum, `SetAppendCMv40()` (cache-invalidating
  setter), `m_append_cmv40` member, `SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND`, settings.xml entry at
  line 4544, `UpdateAppendCMv40SettingCache()` in `DVDVideoCodecAmlogic`, and runtime visibility
  promotion in `CDolbyVisionAML::set_dv_settings_visible()`.

**The only genuinely new primitive** is a libdovi function to remove CMv4.0 metadata; the current
patched libdovi only exposes the *add*.

## Decisions

| Decision | Choice |
|---|---|
| Strip implementation | Approach A — new libdovi patch function (symmetric with the add; libdovi owns RBSP/CRC/serialization) |
| Setting shape | Boolean on/off |
| Strip vs append both enabled | Strip wins in code (skip append silently); documented in code/commit |
| Upstream PR target | quietvoid/dovi_tool (canonical libdovi), matching previous libdovi additions |

## Design

### 1. libdovi patch (CoreELEC side) — the one new primitive

New Rust function mirroring `dovi_rpu_add_cmv40_safe_default_metadata`:

```
dovi_rpu_remove_cmv40_metadata(opaque) -> i32
```

- Removes the CMv4.0 extension metadata blocks from `vdr_dm_data`: **L3, L8, L9, L10, L11, L254**,
  leaving CMv2.9 levels (L1/L2/L4/L5/L6) intact, and fixes block counts/lengths.
- Returns `1` = removed, `0` = nothing to remove (no CMv4.0 present), `-1` = error.
- Re-serialization is done by the existing `dovi_write_unspec62_nalu` — libdovi recomputes RBSP and
  CRC. No hand-rolled bit surgery.
- Patch lives in CoreELEC `source-patches/` (same place as the add patch, since the prebuilt has no
  Rust source for `patches/` to apply to), adapted for the in-tree libdovi version (3.3.1 at port
  time).
- Prebuilt arm tarball rebuilt per the existing package.mk tutorial (system rustup + cargo-c + CE
  cross-compiler).

### 2. Upstream PR — quietvoid/dovi_tool

Submit `dovi_rpu_remove_cmv40_metadata` (and its mirror, if not already upstream) as a PR to the
canonical libdovi/dovi_tool repo, the same way previous libdovi additions were contributed. The
CoreELEC `source-patches/` copy carries it until/if merged upstream.

### 3. Kodi C++ — `CBitstreamConverter` (`BitstreamConverter.h` / `.cpp`)

- New member `bool m_strip_cmv40` (init `false` in the constructor list alongside `m_append_cmv40`).
- New setter mirroring `SetAppendCMv40`:
  ```cpp
  void SetStripCMv40(bool value) {
    if (m_strip_cmv40 != value) InvalidateDoViCache();
    m_strip_cmv40 = value;
  }
  ```
- New `inline bool StripCMv40(...)` helper next to `AppendCMv40()` in the anonymous namespace:
  - Gated on `vdrDmData->dm_data.level254` (only act when CMv4.0 is actually present); returns
    `false` otherwise.
  - Calls `dovi_rpu_remove_cmv40_metadata(opaque)`; on `== 1`, `dovi_write_unspec62_nalu` and update
    `nalBuf`/`nalSize` (same shape as `AppendCMv40`).
- Wire into `ProcessDoViRpu()` in the same single-parse block where `AppendCMv40` is called:
  - **Mutual exclusion (strip wins):** if `m_strip_cmv40`, call `StripCMv40()` and **do not** call
    `AppendCMv40()`. (Append-then-strip is nonsense; strip-then-append would re-add what we removed.)
  - On success, tag `meta_version` so the OSD/log reflect the down-convert (mirror the append "V"
    prefix): prefix with `"C29 "` if not already present. Update `DataCacheCore`.
  - First-frame `CLog::Log(LOGINFO, ...)`: "CMv4.0 stripped to CMv2.9".

### 4. Kodi C++ — `CDVDVideoCodecAmlogic` (`DVDVideoCodecAmlogic.cpp` / `.h`)

Mirror the append wiring exactly:
- `std::atomic<bool> m_stripCMv40Setting` cache + `bool m_stripCMv40Applied`.
- `UpdateStripCMv40SettingCache()` reading `SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP`, called at the
  same three sites as `UpdateAppendCMv40SettingCache()`: constructor/init, settings-changed handler,
  and the playback apply path (~line 432).
- Apply via `m_bitstream->SetStripCMv40(...)` guarded by an `Applied != Setting` check, same as
  `SetAppendCMv40`.

### 5. Settings

- `xbmc/settings/Settings.h`: new constant
  ```cpp
  static constexpr auto SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP =
      "coreelec.amlogic.dolbyvision.cmv40.strip";
  ```
- `system/settings/settings.xml`: new setting immediately after `cmv40.append` (after line 4562),
  **boolean toggle**, copying the append entry's `<requirement>HAVE_AMCODEC</requirement>`,
  `<visible>false</visible>`, `<level>2</level>`, `parent`, and the three `<dependency type="visible">`
  rules (mode !is 2, video.processor is 0, type is 0). `<default>false</default>`, new label/help
  ids, `<control type="boolean" />`.
- Runtime visibility promotion: add the strip setting to the list in
  `CDolbyVisionAML::set_dv_settings_visible()` (same as `cmv40.append`) so the addon's JSON-RPC
  `SetSettingValue` is not rejected by the `IsVisible()` gate.

### 6. Strings (en_gb + de_de)

Next free ids in the cmv40 cluster: **60328** (label), **60329** (help). Add to
`addons/resource.language.en_gb/resources/strings.po` and the de_de catalog.

- 60328 label (en): "Convert CMv4.0 to CMv2.9"
- 60329 help (en): "Strip CMv4.0 extension blocks from Dolby Vision RPU metadata during playback,
  leaving a CMv2.9-only stream. For old DV TVs that fail to fall back from CMv4.0 (e.g. black screen
  on CMv4.0 content)."

## Known limitations (document in commit + memory)

- Strips to **base** CMv2.9 — drops CMv4.0 L8 trims rather than mapping L8 → L2. Acceptable for the
  lock/black-screen goal; not a full perceptual down-convert.
- Only affects the **native-DV forwarded RPU** path. Irrelevant when the box does VS10 / box-side
  tonemapping (the raw RPU isn't forwarded then).
- **Root cause unconfirmed.** It is not yet proven that the CM version (vs. a profile/level/signaling
  issue) causes the black screen. Requires on-device A/B by the reporter: toggle strip on → does the
  black screen clear? Ship as untested-on-device, consistent with the rest of the DV work.

## Testing

- No practical host unit test for the libdovi/RPU path on this tree; validation is on-device.
- On-device A/B: reporter enables the setting on CMv4.0 content that currently blacks out → expect a
  locked CMv2.9 picture. Confirm the log shows "CMv4.0 stripped to CMv2.9" and the OSD/meta shows the
  "C29" marker.
- Regression check: with the setting off, behavior is byte-identical (the new path is fully gated on
  the setting and on `level254` presence).

## Affected files

- CoreELEC `source-patches/` — new libdovi patch (the remove function)
- quietvoid/dovi_tool — upstream PR (remove function)
- `xbmc/utils/BitstreamConverter.h` / `.cpp` — member, setter, `StripCMv40()`, `ProcessDoViRpu` wiring
- `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp` / `.h` — setting cache + apply
- `xbmc/settings/Settings.h` — setting constant
- `system/settings/settings.xml` — boolean setting
- `xbmc/windowing/amlogic/DolbyVisionAML.cpp` — visibility promotion
- `addons/resource.language.en_gb/resources/strings.po` + de_de — strings
