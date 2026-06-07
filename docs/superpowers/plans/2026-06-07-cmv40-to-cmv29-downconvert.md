# CMv4.0 → CMv2.9 Down-Convert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in setting that strips CMv4.0 extension metadata from Dolby Vision RPUs at playback, producing a clean CMv2.9 stream for old DV TVs that black-screen on CMv4.0 content.

**Architecture:** One new libdovi primitive (`dovi_rpu_remove_cmv40_metadata`, mirror of the existing `dovi_rpu_add_cmv40_safe_default_metadata`) plus a thin Kodi C++ layer that reuses the existing CMv4.0-append plumbing in `ProcessDoViRpu` — a boolean setting, a `StripCMv40()` helper gated on `level254`, strip-wins-over-append precedence, and the standard `DVDVideoCodecAmlogic` setting cache/apply wiring.

**Tech Stack:** Rust (libdovi/dovi_tool), C++ (Kodi/CoreELEC Amlogic VideoPlayer), Kodi settings.xml + gettext .po strings, CoreELEC build (source-patches + prebuilt tarball).

---

## Testing reality (read before starting)

This subsystem has **no host unit-test harness**. The Amlogic DV path only runs on-device, and the libdovi RPU functions run inside a cross-compiled prebuilt. So "TDD with a failing unit test" does not apply here. Verification is:

1. **Compile** — the full CoreELEC image build (uses the separate `xbmc-local` checkout — see memory `skin-p3i-estuary-bundle`/`cmv40-append-port`; changes must be pushed/synced to that checkout before building).
2. **Static checks** — `grep`/read-back that each edit landed and symbols line up.
3. **On-device A/B** — the reporter toggles the setting on CMv4.0 content that currently blacks out and confirms a locked CMv2.9 picture + the expected log line.

Do **not** invent unit tests that cannot run. Each task's "verify" step is a real compile/grep/log check.

## File structure / responsibilities

| File | Responsibility | Change |
|---|---|---|
| dovi_tool (Rust, upstream clone) | New `dovi_rpu_remove_cmv40_metadata` C API + impl | Create (PR) |
| CoreELEC `source-patches/<libdovi>/` | Carry the remove function into the prebuilt | Create patch + rebuild prebuilt |
| `xbmc/settings/Settings.h` | Setting id constant | Modify |
| `system/settings/settings.xml` | Boolean setting entry | Modify |
| `addons/resource.language.en_gb/resources/strings.po` | en label/help | Modify |
| `addons/resource.language.de_de/resources/strings.po` | de label/help | Modify |
| `xbmc/utils/BitstreamConverter.h` | `m_strip_cmv40` member + `SetStripCMv40()` setter | Modify |
| `xbmc/utils/BitstreamConverter.cpp` | ctor init, `StripCMv40()` helper, `ProcessDoViRpu` wiring + meta tag + log | Modify |
| `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h` | strip setting cache members + method decl | Modify |
| `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp` | cache update, callback reg, apply at 2 sites | Modify |
| `xbmc/windowing/amlogic/DolbyVisionAML.cpp` | runtime visibility promotion | Modify |

**Ordering matters:** the libdovi symbol must exist before the Kodi code that calls it will link. Do Task 1–2 first; Kodi tasks (3–8) can be written in any order but only *build* once the prebuilt has the symbol.

---

### Task 1: libdovi `dovi_rpu_remove_cmv40_metadata` (Rust + upstream PR)

**Files:**
- Clone/fork: `quietvoid/dovi_tool` (not on disk yet — `git clone` it; PR target is upstream)
- Modify: the libdovi C-API crate (`dolby_vision/src/c_structs/` + the capi module that defines `dovi_rpu_add_cmv40_safe_default_metadata`)

> **Source of truth:** open the existing `dovi_rpu_add_cmv40_safe_default_metadata` in the dovi_tool tree and **mirror its structure exactly** (signature style, opaque handling, error codes, where it lives). The code below is the intended behavior; align names/paths to the existing add function.

- [ ] **Step 1: Locate the existing add function**

```bash
git clone https://github.com/quietvoid/dovi_tool && cd dovi_tool
grep -rn "add_cmv40_safe_default_metadata\|ext_metadata_blocks\|ExtMetadataBlock" dolby_vision/src
```

Expected: finds the add capi function and the `vdr_dm_data` ext-block model (`ExtMetadataBlock` enum, `vdr_dm_data.metadata.ext_metadata_blocks` Vec).

- [ ] **Step 2: Implement the remove function (mirror the add)**

Add next to the add function. Behavior: remove all CMv4.0 ext metadata blocks (Level 3, 8, 9, 10, 11, 254), leave CMv2.9 (L1/L2/L4/L5/L6), recompute counts, leave the RPU re-serializable by `dovi_write_unspec62_nalu`.

```rust
/// Remove CMv4.0 extension metadata blocks from the RPU's vdr_dm_data,
/// leaving a clean CMv2.9 payload. Mirror of dovi_rpu_add_cmv40_safe_default_metadata.
///
/// Returns: 1 = removed, 0 = nothing to remove, -1 = error.
#[no_mangle]
pub unsafe extern "C" fn dovi_rpu_remove_cmv40_metadata(ptr: *mut RpuOpaque) -> i32 {
    let opaque = match ptr.as_mut() {
        Some(o) => o,
        None => return -1,
    };
    let rpu = match opaque.rpu.as_mut() {
        Some(r) => r,
        None => return -1,
    };
    let vdr = match rpu.vdr_dm_data.as_mut() {
        Some(v) => v,
        None => return 0, // no DM data => nothing CMv4.0 to remove
    };

    let before = vdr.metadata.ext_metadata_blocks.len();
    // CMv4.0-only levels. CMv2.9 keeps L1/L2/L4/L5/L6.
    vdr.metadata.ext_metadata_blocks.retain(|b| {
        !matches!(
            b,
            ExtMetadataBlock::Level3(_)
                | ExtMetadataBlock::Level8(_)
                | ExtMetadataBlock::Level9(_)
                | ExtMetadataBlock::Level10(_)
                | ExtMetadataBlock::Level11(_)
                | ExtMetadataBlock::Level254(_)
        )
    });
    let removed = before - vdr.metadata.ext_metadata_blocks.len();

    // Recompute the ext-block count/length bookkeeping the same way the add path does.
    vdr.metadata.update_extension_metadata_block_info();

    if removed > 0 { 1 } else { 0 }
}
```

> If the existing add function calls a different recompute helper (e.g. not `update_extension_metadata_block_info`), use that same helper. Match the existing code — do not introduce a new pattern.

- [ ] **Step 3: Build the crate**

```bash
cargo build --release -p dolby_vision
```

Expected: compiles. If `ExtMetadataBlock::Level10` doesn't exist in this version, drop it from the match (only remove variants that exist in the pinned version — the add function's variant set is the authority).

- [ ] **Step 4: Run dovi_tool's own tests (if the crate has RPU round-trip tests)**

```bash
cargo test -p dolby_vision
```

Expected: PASS. (No new test required if the crate has none for the add function — parity with the add function.)

- [ ] **Step 5: Commit + open upstream PR**

```bash
git checkout -b feat/remove-cmv40-metadata
git add -A
git -c user.email=1359593+pannal@users.noreply.github.com commit -m "capi: add dovi_rpu_remove_cmv40_metadata (strip CMv4.0 to CMv2.9)"
```

Open a PR to `quietvoid/dovi_tool` describing it as the inverse of `dovi_rpu_add_cmv40_safe_default_metadata` for down-converting RPUs to CMv2.9. **Do not push from automation — report the branch + suggested `gh pr create` command to panni.**

---

### Task 2: CoreELEC source-patch + prebuilt rebuild

**Files:**
- Create: `CoreELEC/projects/Amlogic-ce/source-patches/<libdovi-pkg>/NNN-remove-cmv40-metadata.patch`
- Rebuild: the libdovi prebuilt arm tarball (per the package.mk tutorial)

> Mirror exactly how the add function's patch + prebuilt were done (memory `cmv40-append-port`: patch lives in `source-patches/`, **not** `patches/`; prebuilt rebuilt with system rustup + cargo-c + CE cross-compiler; tarball needs the `libdovi-arm-<ver>/` prefix).

- [ ] **Step 1: Generate the patch from the Task 1 diff**

```bash
cd dovi_tool
git diff <base>..feat/remove-cmv40-metadata > /tmp/remove-cmv40.patch
```

Adapt it to the pinned libdovi version used by the CoreELEC prebuilt (the add patch already targets that version — match its context).

- [ ] **Step 2: Drop it into source-patches and rebuild the prebuilt**

Follow the rebuild tutorial in the libdovi `package.mk` (the same one used for the add). Produce the patched arm tarball.

- [ ] **Step 3: Verify the symbol is in the prebuilt**

```bash
nm -D <prebuilt>/lib/libdovi.so 2>/dev/null | grep dovi_rpu_remove_cmv40_metadata
```

Expected: one `T dovi_rpu_remove_cmv40_metadata` line. If the prebuilt is a static `.a`, use `nm` on the archive instead.

- [ ] **Step 4: Commit (CoreELEC repo)**

```bash
git -c user.email=1359593+pannal@users.noreply.github.com commit -am "libdovi: add remove_cmv40_metadata source-patch + rebuilt prebuilt"
```

Report the push command to panni; do not push.

---

### Task 3: Kodi setting id constant

**Files:**
- Modify: `xbmc/settings/Settings.h:503` (right after `SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND`)

- [ ] **Step 1: Add the constant**

After the line:
```cpp
  static constexpr auto SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND = "coreelec.amlogic.dolbyvision.cmv40.append";
```
add:
```cpp
  static constexpr auto SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP = "coreelec.amlogic.dolbyvision.cmv40.strip";
```

- [ ] **Step 2: Verify**

```bash
grep -n "DV_CMV40_STRIP" xbmc/settings/Settings.h
```
Expected: one match.

---

### Task 4: settings.xml entry + strings

**Files:**
- Modify: `system/settings/settings.xml` (after the `cmv40.append` setting block, which ends at line 4562)
- Modify: `addons/resource.language.en_gb/resources/strings.po` (after the `#60324` block, ~line 26000)
- Modify: `addons/resource.language.de_de/resources/strings.po` (after the `#60324` block, ~line 19161)

- [ ] **Step 1: Add the boolean setting after the append block**

Insert immediately after the `</setting>` that closes `coreelec.amlogic.dolbyvision.cmv40.append` (line 4562):

```xml
        <setting id="coreelec.amlogic.dolbyvision.cmv40.strip" type="boolean" label="60328" help="60329" parent="coreelec.amlogic.dolbyvision.type">
          <requirement>HAVE_AMCODEC</requirement>
          <visible>false</visible>
          <level>2</level>
          <default>false</default>
          <dependencies>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.mode" operator="!is">2</dependency>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.video.processor" operator="is">0</dependency>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.type" operator="is">0</dependency>
          </dependencies>
          <control type="boolean" />
        </setting>
```

- [ ] **Step 2: Add en_gb strings after the `#60324` block**

In `addons/resource.language.en_gb/resources/strings.po`, after the `msgctxt "#60324"` ("Always") block, add:

```
msgctxt "#60328"
msgid "Convert CMv4.0 to CMv2.9"
msgstr ""

msgctxt "#60329"
msgid "Strip CMv4.0 extension blocks from Dolby Vision RPU metadata during playback, leaving a CMv2.9-only stream. For old DV TVs that fail to fall back from CMv4.0 (e.g. black screen on CMv4.0 content)."
msgstr ""
```

- [ ] **Step 3: Add de_de strings after the `#60324` block**

In `addons/resource.language.de_de/resources/strings.po`, after the `msgctxt "#60324"` ("Immer") block, add:

```
msgctxt "#60328"
msgid "Convert CMv4.0 to CMv2.9"
msgstr "CMv4.0 zu CMv2.9 konvertieren"

msgctxt "#60329"
msgid "Strip CMv4.0 extension blocks from Dolby Vision RPU metadata during playback, leaving a CMv2.9-only stream. For old DV TVs that fail to fall back from CMv4.0 (e.g. black screen on CMv4.0 content)."
msgstr "CMv4.0-Erweiterungsblöcke während der Wiedergabe aus den Dolby Vision RPU-Metadaten entfernen, sodass ein reiner CMv2.9-Stream verbleibt. Für ältere DV-Fernseher, die nicht korrekt von CMv4.0 zurückfallen (z. B. Schwarzbild bei CMv4.0-Inhalten)."
```

- [ ] **Step 4: Verify**

```bash
grep -n "cmv40.strip" system/settings/settings.xml
grep -n "60328\|60329" addons/resource.language.en_gb/resources/strings.po addons/resource.language.de_de/resources/strings.po
```
Expected: settings.xml 1 match; each .po 2 matches.

---

### Task 5: BitstreamConverter.h — member + setter

**Files:**
- Modify: `xbmc/utils/BitstreamConverter.h:140-143` (setter, after `SetAppendCMv40`)
- Modify: `xbmc/utils/BitstreamConverter.h:252` (member, after `m_append_cmv40`)

- [ ] **Step 1: Add the setter after `SetAppendCMv40`**

After:
```cpp
  void              SetAppendCMv40(enum DOVICMv40Mode value) {
                      if (m_append_cmv40 != value) InvalidateDoViCache();
                      m_append_cmv40 = value;
                    }
```
add:
```cpp
  void              SetStripCMv40(bool value) {
                      if (m_strip_cmv40 != value) InvalidateDoViCache();
                      m_strip_cmv40 = value;
                    }
```

- [ ] **Step 2: Add the member after `m_append_cmv40`**

After line 252 (`enum DOVICMv40Mode m_append_cmv40;`) add:
```cpp
  bool              m_strip_cmv40{false};
```

- [ ] **Step 3: Verify**

```bash
grep -n "SetStripCMv40\|m_strip_cmv40" xbmc/utils/BitstreamConverter.h
```
Expected: 2 matches in the setter + 1 member (the `if` line references it too) — i.e. `m_strip_cmv40` appears 3×, `SetStripCMv40` 1×.

---

### Task 6: BitstreamConverter.cpp — ctor init, `StripCMv40()`, `ProcessDoViRpu` wiring

**Files:**
- Modify: `xbmc/utils/BitstreamConverter.cpp:692` (ctor init, after `m_append_cmv40 = ...`)
- Modify: `xbmc/utils/BitstreamConverter.cpp:618` (new `StripCMv40` helper in the anonymous namespace, after `AppendCMv40`)
- Modify: `xbmc/utils/BitstreamConverter.cpp:1573-1598` (wiring in `ProcessDoViRpu`)

- [ ] **Step 1: Initialise the member in the constructor**

After:
```cpp
  m_append_cmv40 = DOVICMv40Mode::CMV40_NONE;
```
add:
```cpp
  m_strip_cmv40 = false;
```

- [ ] **Step 2: Add the `StripCMv40` helper after `AppendCMv40` (before the closing `} // namespace`)**

Insert after the `AppendCMv40` function (which ends at line 618, before line 620 `} // namespace`):

```cpp
inline bool StripCMv40(const DoviVdrDmData* vdrDmData,
                       DoviRpuOpaque* opaque,
                       uint8_t*& nalBuf,
                       int32_t& nalSize,
                       const DoviData*& rpuData)
{
  if (!vdrDmData || !opaque) return false;

  // Only act when CMv4.0 is actually present (L254 is the CMv4.0 marker).
  if (!vdrDmData->dm_data.level254) return false;

  if (dovi_rpu_remove_cmv40_metadata(opaque) != 1)
    return false;

  rpuData = dovi_write_unspec62_nalu(opaque);
  if (!rpuData) return false;

  nalBuf = const_cast<uint8_t*>(rpuData->data);
  nalSize = static_cast<int32_t>(rpuData->len);
  return true;
}
```

- [ ] **Step 3: Wire into `ProcessDoViRpu` — strip wins over append**

Replace the existing append block (lines 1573-1598):

```cpp
    if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE)
      appended = AppendCMv40(m_append_cmv40, header, vdrDmData, opaque,
                             nal_buf, nal_size, appendRpuData);
```
with:
```cpp
    bool stripped = false;
    if (m_strip_cmv40)
    {
      // Strip wins over append: down-convert CMv4.0 -> CMv2.9 for old DV TVs
      // that fail to fall back. Appending then stripping would be nonsense.
      stripped = StripCMv40(vdrDmData, opaque, nal_buf, nal_size, appendRpuData);
    }
    else if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE)
    {
      appended = AppendCMv40(m_append_cmv40, header, vdrDmData, opaque,
                             nal_buf, nal_size, appendRpuData);
    }
```

Then update the post-processing block (currently lines 1588-1598, the `if (appended)` block) to also tag/log the strip. Replace:
```cpp
    if (appended)
    {
      DOVIStreamMetadata meta = m_dataCacheCore.GetVideoDoViStreamMetadata();
      if (!meta.meta_version.empty() && meta.meta_version[0] != 'V')
      {
        meta.meta_version = "V" + meta.meta_version;
        m_dataCacheCore.SetVideoDoViStreamMetadata(meta);
      }
      if (m_first_frame)
        CLog::Log(LOGINFO, "CBitstreamConverter::ProcessDoViRpu - CMv4.0 extension appended to RPU");
    }
```
with:
```cpp
    if (appended)
    {
      DOVIStreamMetadata meta = m_dataCacheCore.GetVideoDoViStreamMetadata();
      if (!meta.meta_version.empty() && meta.meta_version[0] != 'V')
      {
        meta.meta_version = "V" + meta.meta_version;
        m_dataCacheCore.SetVideoDoViStreamMetadata(meta);
      }
      if (m_first_frame)
        CLog::Log(LOGINFO, "CBitstreamConverter::ProcessDoViRpu - CMv4.0 extension appended to RPU");
    }
    else if (stripped)
    {
      DOVIStreamMetadata meta = m_dataCacheCore.GetVideoDoViStreamMetadata();
      if (meta.meta_version.rfind("C29 ", 0) != 0)
      {
        meta.meta_version = "C29 " + meta.meta_version;
        m_dataCacheCore.SetVideoDoViStreamMetadata(meta);
      }
      if (m_first_frame)
        CLog::Log(LOGINFO, "CBitstreamConverter::ProcessDoViRpu - CMv4.0 stripped to CMv2.9");
    }
```

> Note: `PopulateDoviRpuInfo` (called between the strip and this block on `opaque`) repopulates stream metadata, which is why the `meta_version` tag is applied **after** it — same ordering the append path relies on. The `appendRpuData` out-param is reused for the stripped NAL and is freed at the end of `ProcessDoViRpu` (`if (appendRpuData) dovi_data_free(appendRpuData);`) — no new free needed.

- [ ] **Step 4: Verify the edits landed**

```bash
grep -n "m_strip_cmv40\|StripCMv40\|stripped\|C29 \|CMv4.0 stripped" xbmc/utils/BitstreamConverter.cpp
```
Expected: ctor init (1), helper def + body (several), wiring (several), tag/log (2+).

---

### Task 7: DVDVideoCodecAmlogic — setting cache + apply

**Files:**
- Modify: `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h:114` (method decl) and `:124-125` (members)
- Modify: `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp` — callback reg (82-86), call site (90), new cache fn (after 123), OnSettingChanged (162-164), ApplyDynamicDoViSettings (172-178), Open apply (432-439)

- [ ] **Step 1: Header — declaration + members**

After `void UpdateAppendCMv40SettingCache();` (line 114) add:
```cpp
  void UpdateStripCMv40SettingCache();
```
After:
```cpp
  std::atomic<int> m_appendCMv40ModeSetting{static_cast<int>(DOVICMv40Mode::CMV40_NONE)};
  DOVICMv40Mode m_appendCMv40ModeApplied{DOVICMv40Mode::CMV40_NONE};
```
add:
```cpp
  std::atomic<bool> m_stripCMv40Setting{false};
  bool m_stripCMv40Applied{false};
```

- [ ] **Step 2: Register the callback + call cache update on construct**

In the `RegisterCallback` set (lines 82-86), add the strip id:
```cpp
      settings->RegisterCallback(this, {
        CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_LEVEL5_OVERRIDE,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE
      });
```
After `UpdateAppendCMv40SettingCache();` (line 90) add:
```cpp
  UpdateStripCMv40SettingCache();
```

- [ ] **Step 3: Add the cache-update function after `UpdateAppendCMv40SettingCache` (after line 123)**

```cpp
void CDVDVideoCodecAmlogic::UpdateStripCMv40SettingCache()
{
  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
    {
      // Stripping CMv4.0 -> CMv2.9 only matters on the Display-LED path, where
      // the TV consumes the RPU. On Player-LED / VS10-only we are the tonemapper
      // and the raw RPU is not forwarded, so stripping is wasted work.
      bool strip = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP);
      if (aml_dv_type() != DV_TYPE_DISPLAY_LED)
        strip = false;
      m_stripCMv40Setting.store(strip);
    }
  }
}
```

- [ ] **Step 4: Hook OnSettingChanged**

Extend the append condition (lines 162-164) to also refresh the strip cache:
```cpp
  if (id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE)
    UpdateAppendCMv40SettingCache();
  if (id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE)
    UpdateStripCMv40SettingCache();
```

- [ ] **Step 5: Apply in ApplyDynamicDoViSettings (after the append apply, before the L5 block at line 180)**

After:
```cpp
  const auto mode = static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
  if (mode != m_appendCMv40ModeApplied)
  {
    m_bitstream->SetAppendCMv40(mode);
    m_appendCMv40ModeApplied = mode;
    CLog::Log(LOGINFO, "{}::{} - CMv4.0 append mode changed to {}", __MODULE_NAME__, __FUNCTION__, static_cast<int>(mode));
  }
```
add:
```cpp
  const bool strip = m_stripCMv40Setting.load();
  if (strip != m_stripCMv40Applied)
  {
    m_bitstream->SetStripCMv40(strip);
    m_stripCMv40Applied = strip;
    CLog::Log(LOGINFO, "{}::{} - CMv4.0 strip-to-CMv2.9 changed to {}", __MODULE_NAME__, __FUNCTION__, strip);
  }
```

- [ ] **Step 6: Apply at stream open (after the append apply at lines 432-439)**

After:
```cpp
          auto cmv40Mode = static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
          if (cmv40Mode != DOVICMv40Mode::CMV40_NONE)
          {
            CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - CMv4.0 append mode: {}",
                      __MODULE_NAME__, __FUNCTION__, static_cast<int>(cmv40Mode));
            m_bitstream->SetAppendCMv40(cmv40Mode);
          }
          m_appendCMv40ModeApplied = cmv40Mode;
```
add:
```cpp
          const bool stripCMv40 = m_stripCMv40Setting.load();
          if (stripCMv40)
          {
            CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - CMv4.0 strip-to-CMv2.9 enabled",
                      __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetStripCMv40(true);
          }
          m_stripCMv40Applied = stripCMv40;
```

- [ ] **Step 7: Verify**

```bash
grep -n "StripCMv40\|m_stripCMv40\|CMV40_STRIP\|strip-to-CMv2.9" xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h
```
Expected: header decl + 2 members; cpp callback, call, cache fn, OnSettingChanged, 2 apply sites.

---

### Task 8: DolbyVisionAML — runtime visibility promotion

**Files:**
- Modify: `xbmc/windowing/amlogic/DolbyVisionAML.cpp:613` (in `set_dv_settings_visible`)

- [ ] **Step 1: Add the strip setting to the visibility list**

After:
```cpp
  set_visible(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND, show);
```
add:
```cpp
  set_visible(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP, show);
```

- [ ] **Step 2: Verify**

```bash
grep -n "DV_CMV40_STRIP" xbmc/windowing/amlogic/DolbyVisionAML.cpp
```
Expected: 1 match.

---

### Task 9: Build, on-device verify, commit, memory

- [ ] **Step 1: Sync Kodi changes to the build's xbmc-local checkout**

The CoreELEC build uses the separate `xbmc-local` checkout (memory: `cmv40-append-port`, `skin-p3i-estuary-bundle`). Push this branch and sync it there before building, or the build uses the stale tree (memory: `build-script-pull-reset-footgun`).

- [ ] **Step 2: Full image build**

Build the CoreELEC image with the rebuilt libdovi prebuilt (Task 2) + the Kodi changes.
Expected: links cleanly — `dovi_rpu_remove_cmv40_metadata` resolves from the prebuilt.

- [ ] **Step 3: On-device A/B (reporter)**

On the old DV TV with CMv4.0 content that currently blacks out:
- Setting **off**: confirms current black-screen behavior (baseline).
- Setting **on**: expect a locked CMv2.9 picture. `kodi.log` shows `CMv4.0 stripped to CMv2.9`; OSD/meta shows the `C29` marker.
- Regression: with setting off, behavior is byte-identical (path fully gated on the setting and on `level254`).

- [ ] **Step 4: Commit (Kodi repo)**

```bash
git add xbmc/settings/Settings.h system/settings/settings.xml \
  addons/resource.language.en_gb/resources/strings.po \
  addons/resource.language.de_de/resources/strings.po \
  xbmc/utils/BitstreamConverter.h xbmc/utils/BitstreamConverter.cpp \
  xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h \
  xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp \
  xbmc/windowing/amlogic/DolbyVisionAML.cpp
git -c user.email=1359593+pannal@users.noreply.github.com commit -m "DV: add CMv4.0->CMv2.9 down-convert setting for old DV TVs

Old CMv2.9-only DV TVs can black-screen on CMv4.0 content (fail to fall
back). New opt-in setting coreelec.amlogic.dolbyvision.cmv40.strip strips
CMv4.0 ext blocks (L3/L8/L9/L10/L11/L254) from each RPU at playback via
new libdovi dovi_rpu_remove_cmv40_metadata, leaving a clean CMv2.9 stream.
Strip wins over CMv4.0 append; Display-LED only; meta tagged C29."
```

Report the push command to panni; do not push.

- [ ] **Step 5: Update memory**

Update `memory/cmv40-append-port.md` (or a new `cmv40-to-cmv29-downconvert.md` + MEMORY.md index line) noting: new strip setting, the libdovi remove primitive + upstream PR, strip-wins precedence, Display-LED gating, `C29` marker, and **untested-on-device / root-cause-unconfirmed** status.

---

## Self-review notes

- **Spec coverage:** libdovi remove primitive (T1), upstream PR (T1), CoreELEC patch+prebuilt (T2), setting constant (T3), settings.xml boolean + strings (T4), BitstreamConverter member/setter/helper/wiring/meta-tag/log (T5–T6), DVDVideoCodecAmlogic cache+apply (T7), visibility promotion (T8), build+on-device+limitations (T9). All spec sections mapped.
- **Naming consistency:** `dovi_rpu_remove_cmv40_metadata`, `SetStripCMv40`, `m_strip_cmv40`, `StripCMv40()`, `UpdateStripCMv40SettingCache`, `m_stripCMv40Setting`/`m_stripCMv40Applied`, `SETTING_COREELEC_AMLOGIC_DV_CMV40_STRIP`, marker `C29 `, strings `60328`/`60329` — used identically across all tasks.
- **Display-LED gating:** strip cache zeroed off Display-LED (T7 step 3), matching append's gate — consistent with "native-DV forwarded RPU path only" from the spec.
- **No fabricated tests:** verification is compile + grep + on-device, per the testing-reality note (this subsystem has no host harness).
