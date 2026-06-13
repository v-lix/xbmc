# Dolby Vision Smart CMv4.0 Bypass — Full Implementation Guide

## Feature Goal

Add a new **"Smart"** (value `3`) option to the existing CoreELEC
`coreelec.amlogic.dolbyvision.cmv40.append` setting. When selected, on every Dolby
Vision frame Kodi evaluates three criteria in priority order:

- **No L2 trims** (CMv2.9 stream without L2 blocks) → always append CMv4.0 (same as `CMV40_NO_L2`)
- **Content signal peak > display max nits × (1 + threshold/100)** → bypass CMv4.0 append
- **Otherwise** → append CMv4.0 as normal

Content peak is read from the RPU's `source_max_pq` field (actual encoded signal
maximum, converted via `max_pq_to_nits()`) rather than from Level 6 mastering display
metadata, so the comparison reflects what the content actually contains rather than
the capability of the reference monitor it was graded on. Display nits come from the
EDID VSVDB/HGIG maximum luminance read at startup.

A companion setting `coreelec.amlogic.dolbyvision.cmv40.smart.threshold` (default 20%)
controls how far the content must exceed the display's peak brightness before bypass
triggers. At 20%, a 1000-nit display will still append CMv4.0 for content up to 1200
nits, only bypassing when the content clearly exceeds what the display can reproduce.
Set to 0% to bypass whenever content exceeds display peak at all.

---

## Use case — what this actually compares, and who it's for

Read this before deciding whether to enable Smart.

### The comparison is narrower than "CMv4.0 vs CMv2.9"

`AppendCMv40()` returns early when the RPU already carries L254 (the CMv4.0 marker),
so **append — and therefore Smart — never touches native CMv4.0 content**. It only ever
takes a **CMv2.9-authored** stream and bolts on a *synthetic* CMv4.0 container built by
`dovi_rpu_add_cmv40_safe_default_metadata()`. The decision Smart governs is always the
same picture data played two ways:

| | Tonemapper the TV runs | Trims steering it |
|---|---|---|
| **Leave as native CMv2.9** | TV's CMv2.9 path | the colorist's **authored L2** trim |
| **CMv4.0 append** | TV's CMv4.0 path | **generic default** L8/L9/L11/L254 we synthesize |

L1 stats and the colorist's L2 intent are identical on both sides. What changes is which
processing path the TV runs, and that the v4 path is steered by *default* trims, not
authored ones.

### Why append usually wins

The CMv4.0 path is the better tonemapper (cleaner highlight roll-off, better mid-tone
preservation) and gets extra context from the synthesized L11 (content intent) and L9
(primaries). Even with neutral default trims it is generally equal-or-better, which is
why "just append" (`Always`) is the sensible default and remains so.

### The one regime where leaving it CMv2.9 can win

The synthetic L8 is *generic* — no authored guidance for how this particular title should
roll off onto a dimmer display. When little tonemapping is needed (content peak ≈ display
peak) that is irrelevant. When **heavy** tonemapping is needed (content peak ≫ display
peak) the roll-off curve does most of the work, and the v4 path is extrapolating from a
default while the CMv2.9 path follows a human-set L2 trim — so the older-but-authored path
can be more predictable in specular highlights / near clip. That single edge is all Smart
buys. It is empirical/taste, not proven; treat it as a hypothesis with a knob.

### Two things keep Smart conservative (biased toward appending)

1. It compares against **`source_max_pq` — the actual encoded signal peak**, not the
   mastering-display max (L6). Many "4000-nit" titles never push a pixel past ~1500–2000;
   those read low and append anyway.
2. Threshold is **display nits × (1 + pct/100)**, default +20%.

At the 20% default:
- Display advertises **700** → threshold 840 → most real HDR bypasses → CMv2.9.
- Display advertises **4000** → threshold 4800 → nothing bypasses → identical to `Always`.

### Who it's for

- **Dim DV TVs (≈600–800 nit, budget panels):** the intended audience. Almost everything
  needs heavy tonemapping, the generic synthetic trim is least trustworthy there, and these
  sets are also the ones most likely to mishandle synthetic CMv4.0 — so falling back to the
  authored CMv2.9 roll-off is the safer bet.
- **High-nit flagships (e.g. LG G5):** marginal — Smart barely fires. The "display nits"
  is the value the **EDID VSVDB advertises**, auto-read at startup (not necessarily the
  panel's true small-window peak — some sets advertise conservatively, so check yours). A
  LG G5 advertises ≈**2600 nits** → 20% threshold ≈ **3120 nits**, which only the actual
  *encoded* peak of the very top-end 4000-nit masters exceeds; everything else appends. So on
  a G5, `Smart` and `Always` (=2) are effectively the same picture — prefer **`Always`** if you
  want CMv4.0 guaranteed on every title with zero surprises. (The caveat only bites on panels
  that advertise *low*: there Smart can bypass append on premium content the display could
  have tonemapped well via the v4 path — raise the threshold or use `Always`.)

### How to see where the line sits

Check `coreelec.amlogic.dolbyvision.vsvdb.max.luminance` (the advertised display peak), and
the first DV frame in Smart mode logs `... Smart CMv4.0: content {N}nits display {D}nits
threshold {T}nits ({pct}%) -> {decision}`. That tells you exactly which side of the line each
title lands on, so you can tune the threshold empirically.

> All of the above is on the Display-LED path only (the TV is the tonemapper); append/strip/
> Smart are forced off otherwise.

---

## As shipped — deltas from this guide

This guide was drafted against a tree that predated the **CMv4.0→CMv2.9 strip**
feature (`coreelec.amlogic.dolbyvision.cmv40.strip`). The implementation as committed
adapts to that feature; the concrete deltas:

- **String IDs.** Strip already owns `60328`/`60329`. The Smart threshold setting uses
  **`60332`** (label) / **`60333`** (help) instead. The Smart spinner option reuses the
  orphaned **`60325`** ("Auto" → "Smart"), as the guide intended.
- **ProcessDoViRpu placement.** The Smart logic lives inside the existing
  `else if (m_append_cmv40 != CMV40_NONE)` branch (the `if (m_strip_cmv40)` branch still
  takes priority — *strip wins over append/smart*), not as a standalone replacement of
  the `AppendCMv40` call.
- **Threshold setting dependencies.** The XML carries the full DV visibility triple
  (`mode != 2`, `video.processor is 0`, `type is 0`) **and** `cmv40.append is 3`, matching
  its sibling append/strip settings — not the single `append == 3` dependency the snippet
  in Change 6b shows.
- **Codec-layer fields are atomic.** `m_smartDisplayNits` / `m_smartThresholdPct` in
  `CDVDVideoCodecAmlogic` are `std::atomic<int>` (not plain `int`), matching the
  `m_appendCMv40ModeSetting` / `m_stripCMv40Setting` caches. They are written on the
  settings thread (`UpdateAppendCMv40SettingCache` via `OnSettingChanged`) and read on the
  decode thread (`ApplyDynamicDoViSettings` / `Open`). They are stored **before** the
  gating `m_appendCMv40ModeSetting` store so a decode-thread reader that observes
  `CMV40_SMART` is guaranteed to observe coherent nits/threshold (seq_cst publication).
  The bitstream-layer fields (`m_smart_display_nits` etc.) stay plain members — all
  setter calls and `ProcessDoViRpu` reads run on the single VideoPlayerVideo decode thread.

---

## Change 1 — `xbmc/utils/BitstreamConverter.h`

### 1a. Add `CMV40_SMART` to the enum

```cpp
enum DOVICMv40Mode : int
{
  CMV40_NONE = 0,
  CMV40_NO_L2,
  CMV40_ALWAYS,
  CMV40_SMART,  // append unless content signal peak (source_max_pq) > display EDID max nits
};
```

### 1b. Update `SetAppendCMv40()` and add new setters

```cpp
  void              SetAppendCMv40(enum DOVICMv40Mode value) {
                      if (m_append_cmv40 != value) InvalidateDoViCache();
                      m_append_cmv40 = value;
                      m_smart_last_effective = CMV40_SMART;
                    }
  void              SetSmartBypassDisplayNits(int nits) { m_smart_display_nits = nits; }
  void              SetSmartBypassThresholdPct(int pct) { m_smart_threshold_pct = pct; }
```

`m_smart_last_effective` reset to `CMV40_SMART` (the sentinel) ensures the first RPU
frame of every file open logs its decision, even if the decision is unchanged from the
previous file.

### 1c. Add three private member fields

```cpp
  enum DOVICMv40Mode m_append_cmv40;
  int               m_smart_display_nits{0};
  int               m_smart_threshold_pct{20};
  DOVICMv40Mode     m_smart_last_effective{CMV40_SMART};
```

---

## Change 2 — `xbmc/utils/BitstreamConverter.cpp`

### 2a. Replace the `AppendCMv40` call with smart-bypass logic

```cpp
    if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE)
    {
      DOVICMv40Mode effectiveMode = m_append_cmv40;
      if (m_append_cmv40 == DOVICMv40Mode::CMV40_SMART)
      {
        bool level2IsEmpty = !vdrDmData || (vdrDmData->dm_data.level2.len == 0);
        bool hasData = (m_smart_display_nits > 0 && vdrDmData);
        int contentNits = hasData
            ? max_pq_to_nits(static_cast<int>(vdrDmData->source_max_pq))
            : 0;
        // Bypass only when the stream has L2 trims (not a CMv2.9-no-L2 upgrade candidate)
        // and the content signal peaks beyond display capability plus threshold headroom.
        int threshold = m_smart_display_nits * (100 + m_smart_threshold_pct) / 100;
        bool bypass = !level2IsEmpty && hasData && (contentNits > threshold);
        effectiveMode = bypass ? DOVICMv40Mode::CMV40_NONE : DOVICMv40Mode::CMV40_ALWAYS;
        if (effectiveMode != m_smart_last_effective)
        {
          if (level2IsEmpty)
            CLog::Log(LOGINFO, "CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: "
                      "no L2 trims, appending CMv4.0");
          else if (!hasData)
            CLog::Log(LOGINFO, "CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: "
                      "display nits unavailable, defaulting to append");
          else
            CLog::Log(LOGINFO,
                      "CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: "
                      "content {}nits display {}nits threshold {}nits ({}%) -> {}",
                      contentNits, m_smart_display_nits, threshold, m_smart_threshold_pct,
                      bypass ? "bypass (no append)" : "append CMv4.0");
          m_smart_last_effective = effectiveMode;
        }
      }
      if (effectiveMode != DOVICMv40Mode::CMV40_NONE)
        appended = AppendCMv40(effectiveMode, header, vdrDmData, opaque,
                               nal_buf, nal_size, appendRpuData);
    }
```

**Logic:** Three criteria evaluated per-frame:

1. **`level2IsEmpty`**: no L2 trims → always upgrade (same as `CMV40_NO_L2`). Takes
   priority over nits comparison; applies to CMv2.9 streams that lack L2 blocks.

2. **`hasData`**: requires non-zero `m_smart_display_nits` and non-null `vdrDmData`.
   `source_max_pq` is a mandatory field on every Dolby Vision RPU (unlike Level 6 which
   is optional and pointer-guarded). If display nits are not yet available from EDID,
   defaults to `CMV40_ALWAYS` (safe — append).

3. **Threshold comparison**: `threshold = displayNits × (100 + pct) / 100`. Bypass only
   when `contentNits > threshold`. At 20% default: a 1000-nit display bypasses only
   when content exceeds 1200 nits. At 0%: bypasses whenever content exceeds display peak.

The log fires only when `effectiveMode` changes (`m_smart_last_effective` sentinel). In
practice `source_max_pq` is constant within a stream so the log fires once per file open.

---

## Change 3 — `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h`

```cpp
  std::atomic<int> m_appendCMv40ModeSetting{static_cast<int>(DOVICMv40Mode::CMV40_NONE)};
  DOVICMv40Mode m_appendCMv40ModeApplied{DOVICMv40Mode::CMV40_NONE};
  int m_smartDisplayNits{0};
  int m_smartThresholdPct{20};
```

---

## Change 4 — `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp`

### 4a. Register callback for threshold setting

```cpp
      settings->RegisterCallback(this, {
        CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_LEVEL5_OVERRIDE,
        CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE
      });
```

### 4b. `UpdateAppendCMv40SettingCache()` — cache both nits and threshold

```cpp
      m_appendCMv40ModeSetting.store(cmv40);
      if (static_cast<DOVICMv40Mode>(cmv40) == DOVICMv40Mode::CMV40_SMART)
      {
        m_smartDisplayNits = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM);
        m_smartThresholdPct = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD);
      }
```

`SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM` is the integer setting populated by
`DolbyVisionAML.cpp` from the display's EDID VSVDB/HGIG maximum luminance (in nits).

### 4c. `OnSettingChanged()` — also react to threshold changes

```cpp
  if (id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE)
    UpdateAppendCMv40SettingCache();
```

### 4d. `ApplyDynamicDoViSettings()` — push both values before mode change

```cpp
    if (mode == DOVICMv40Mode::CMV40_SMART)
    {
      m_bitstream->SetSmartBypassDisplayNits(m_smartDisplayNits);
      m_bitstream->SetSmartBypassThresholdPct(m_smartThresholdPct);
    }
    m_bitstream->SetAppendCMv40(mode);
```

### 4e. `Open()` — push both values and log on stream open

```cpp
            if (cmv40Mode == DOVICMv40Mode::CMV40_SMART)
            {
              m_bitstream->SetSmartBypassDisplayNits(m_smartDisplayNits);
              m_bitstream->SetSmartBypassThresholdPct(m_smartThresholdPct);
              CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - Smart CMv4.0 bypass display {}nits threshold {}%",
                        __MODULE_NAME__, __FUNCTION__, m_smartDisplayNits, m_smartThresholdPct);
            }
```

Both setters must be called **before** `SetAppendCMv40` so the values are set when
`SetAppendCMv40` resets the logging sentinel and the first frame decision uses them.

---

## Change 5 — `xbmc/settings/Settings.h`

```cpp
  static constexpr auto SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND = "coreelec.amlogic.dolbyvision.cmv40.append";
  static constexpr auto SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD = "coreelec.amlogic.dolbyvision.cmv40.smart.threshold";
```

---

## Change 6 — `xbmc/system/settings/settings.xml`

### 6a. Add option 3 to the CMv4.0 append spinner

```xml
          <constraints>
            <options>
              <option label="60322">0</option>
              <option label="60323">1</option>
              <option label="60324">2</option>
              <option label="60325">3</option>
            </options>
          </constraints>
```

### 6b. Add the threshold setting after the CMv4.0 append setting

```xml
        <setting id="coreelec.amlogic.dolbyvision.cmv40.smart.threshold" type="integer" label="60332" help="60333" parent="coreelec.amlogic.dolbyvision.cmv40.append">
          <requirement>HAVE_AMCODEC</requirement>
          <visible>false</visible>
          <level>2</level>
          <default>20</default>
          <dependencies>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.mode" operator="!is">2</dependency>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.video.processor" operator="is">0</dependency>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.type" operator="is">0</dependency>
            <dependency type="visible" setting="coreelec.amlogic.dolbyvision.cmv40.append" operator="is">3</dependency>
          </dependencies>
          <constraints>
            <minimum>0</minimum>
            <maximum>50</maximum>
          </constraints>
          <control type="edit" format="integer" />
        </setting>
```

The setting starts as `visible=false` and is promoted by `set_dv_settings_visible()`.
The XML dependency on `cmv40.append == 3` provides the conditional hide/show when the
user changes the CMv4.0 mode in the UI.

---

## Change 7 — `xbmc/addons/resource.language.en_gb/resources/strings.po`

```po
#. CMv4.0 smart bypass threshold
#: system/settings/settings.xml
msgctxt "#60332"
msgid "CMv4.0 smart bypass threshold (%)"
msgstr ""

msgctxt "#60333"
msgid "Percentage above the display's EDID peak brightness at which CMv4.0 append is bypassed. At 0% any content mastered above display peak triggers bypass; at 20% the content must exceed display peak by at least 20% (e.g. 1200 nits on a 1000-nit display). Only applies when CMv4.0 mode is Smart."
msgstr ""
```

---

## Change 8 — `xbmc/windowing/amlogic/DolbyVisionAML.cpp`

In `set_dv_settings_visible()`, after the CMv4.0 append line:

```cpp
  set_visible(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND, show);
  set_visible(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD, show);
```

---

## Setting Values Reference

| Value | Label | Behavior |
|---|---|---|
| `0` | Off | Never append CMv4.0 |
| `1` | CMv2.9 without L2 trims | Append CMv4.0 only when stream lacks L2 blocks |
| `2` | Always | Always append CMv4.0 |
| `3` | Smart | No-L2 streams always upgraded; otherwise append if content ≤ display×(1+threshold/100) nits, bypass if above |

---

## Data Flow

```
Display EDID read at startup
  └─ DolbyVisionAML.cpp
       └─ SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM  (integer, nits)
            └─ DVDVideoCodecAmlogic::UpdateAppendCMv40SettingCache()
                 └─ m_smartDisplayNits, m_smartThresholdPct
                      └─ CBitstreamConverter::SetSmartBypassDisplayNits()
                         CBitstreamConverter::SetSmartBypassThresholdPct()
                           └─ m_smart_display_nits, m_smart_threshold_pct

Every DV frame
  └─ CBitstreamConverter::ProcessDoViRpu()
       └─ vdrDmData->source_max_pq  (12-bit PQ) → max_pq_to_nits() → contentNits
            threshold = displayNits × (100 + pct) / 100
            no L2 trims           →  effectiveMode = CMV40_ALWAYS (always upgrade)
            display nits == 0     →  effectiveMode = CMV40_ALWAYS (safe fallback)
            contentNits > threshold → effectiveMode = CMV40_NONE   (bypass)
            contentNits ≤ threshold → effectiveMode = CMV40_ALWAYS (append)
            log emitted only when effectiveMode differs from m_smart_last_effective
```

---

## Debugging

**Kodi log** (`kodi.log`, LOGINFO):
```
CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: no L2 trims, appending CMv4.0
CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: content 4000nits display 700nits threshold 840nits (20%) -> bypass (no append)
CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: content 1000nits display 1400nits threshold 1680nits (20%) -> append CMv4.0
CBitstreamConverter::ProcessDoViRpu - Smart CMv4.0: display nits unavailable, defaulting to append
```

Each line is emitted only when the resolved mode changes (once per file in normal use).

---

## Known Limitations

- **`source_max_pq` is always present**: Unlike Level 6 (which is an optional pointer),
  `source_max_pq` is a mandatory scalar field on every Dolby Vision RPU. The only fallback
  path is when `m_smart_display_nits` is 0 (EDID not yet available), which defaults to
  `CMV40_ALWAYS` (append) until display nits are pushed from the codec layer.
- **Threshold unit is percent, not nits**: The setting value is a percentage (0–50).
  The actual nits threshold is computed as `displayNits × (100 + pct) / 100` at runtime.
  The computed threshold is logged alongside the decision for easy debugging.
