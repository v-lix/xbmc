# Codec Logos → Active Area — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let codec logos slide down out of the DV letterbox bar into the visible active area, default-on, in both skins.

**Architecture:** One new resolution-independent Kodi infolabel (`video.dovi.active.area.class`, 0–3) bucketed in C++ from the effective L5 top offset ÷ coded height. Each skin reads that class in conditional `slide` animations on its codec-logo group, gated by `[!opt-out-setting | restriction-setting] + class≠0`. Pure XML on the skin side except the one infolabel.

**Tech Stack:** Kodi C++ (guilib GUIInfo), Kodi skin XML (Estuary-derived), gettext `.po` strings.

---

## Testing note (read first)

This codebase has **no automated test harness** for GUIInfo infolabels or skin XML, and the feature is **CoreELEC/Amlogic-only** (DV active area), so it is not Windows-testable. TDD is adapted as follows:

- **XML edits:** verify well-formedness with `xmllint --noout <file>` (catches the malformed-XML class of failure that has crashed Kodi before) + `grep` the inserted content.
- **C++ edits:** verify by `grep` of the new symbol/token in all three files; the real compile happens in the CoreELEC cross-build (panni builds it). Do **not** attempt to build Kodi here.
- **Behavioral verification:** on-device A/B on the AM6B+ per the spec's Testing section.
- **Commits:** author/committer email `1359593+pannal@users.noreply.github.com`, no co-author. **Never `git push`** — panni pushes (skin repos are shared/test branches). Each repo commits on its current branch (xbmc `aml-4.9-21.3_dev`, skin.plextuary `omega-cpm`, skin.p3i.estuary `master`).

Repo roots:
- xbmc: `/home/panni/xbmc` (== `/mnt/d/data/___small/xbmc`, same `.git`)
- skin.plextuary: `/mnt/d/data/___small/skin.plextuary`
- skin.p3i.estuary: `/home/panni/skin.p3i.estuary`

---

## Task 1: C++ — `video.dovi.active.area.class` infolabel

**Files:**
- Modify: `/home/panni/xbmc/xbmc/guilib/guiinfo/GUIInfoLabels.h` (after line 817)
- Modify: `/home/panni/xbmc/xbmc/GUIInfoManager.cpp` (after line 1282)
- Modify: `/home/panni/xbmc/xbmc/guilib/guiinfo/PlayerGUIInfo.cpp` (the L5 case block, ~708–772)

- [ ] **Step 1: Define the infolabel constant**

In `GUIInfoLabels.h`, immediately after:

```cpp
#define PLAYER_PROCESS_VIDEO_DOVI_L5_DETECT_STATE (PLAYER_PROCESS + 86)
```

add:

```cpp
#define PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS (PLAYER_PROCESS + 87)
```

- [ ] **Step 2: Map the skin token**

In `GUIInfoManager.cpp`, immediately after:

```cpp
                                  {"video.dovi.l5.detect.state", PLAYER_PROCESS_VIDEO_DOVI_L5_DETECT_STATE },
```

add:

```cpp
                                  {"video.dovi.active.area.class", PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS },
```

- [ ] **Step 3: Add the case to the existing L5 block's outer case list**

In `PlayerGUIInfo.cpp`, the block currently begins:

```cpp
    case PLAYER_PROCESS_VIDEO_DOVI_L5_DETECTED:
    {
      /* Prefer source L5 when it has non-zero offsets. Only fall back to
```

Insert the new case so the block also handles it (it reuses the block's `hasL5` and `fTop`):

```cpp
    case PLAYER_PROCESS_VIDEO_DOVI_L5_DETECTED:
    case PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS:
    {
      /* Prefer source L5 when it has non-zero offsets. Only fall back to
```

- [ ] **Step 4: Compute the class in the block's inner switch**

In the same block, the inner switch ends:

```cpp
        case PLAYER_PROCESS_VIDEO_DOVI_L5_DETECTED:
          value = std::to_string(detected); break;
        default: break;
      }
```

Insert the new branch before `default: break;`:

```cpp
        case PLAYER_PROCESS_VIDEO_DOVI_L5_DETECTED:
          value = std::to_string(detected); break;
        case PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS:
        {
          /* Resolution-independent aspect class from the effective top bar:
           * bucket top/height so 1080p and 2160p of the same aspect agree. */
          int videoHeight = CServiceBroker::GetDataCacheCore().GetVideoHeight();
          int cls = 0;
          if (hasL5 && videoHeight > 0)
          {
            float frac = static_cast<float>(fTop) / static_cast<float>(videoHeight);
            if (frac >= 0.11f)
              cls = 3;
            else if (frac >= 0.08f)
              cls = 2;
            else if (frac >= 0.04f)
              cls = 1;
          }
          value = std::to_string(cls);
          break;
        }
        default: break;
      }
```

- [ ] **Step 5: Verify presence in all three files**

Run:
```bash
grep -n "PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS" \
  /home/panni/xbmc/xbmc/guilib/guiinfo/GUIInfoLabels.h \
  /home/panni/xbmc/xbmc/GUIInfoManager.cpp \
  /home/panni/xbmc/xbmc/guilib/guiinfo/PlayerGUIInfo.cpp
grep -n "video.dovi.active.area.class" /home/panni/xbmc/xbmc/GUIInfoManager.cpp
```
Expected: the constant in all three files (define once in .h, token once in GUIInfoManager, twice in PlayerGUIInfo — outer case + inner switch), and the token string present once.

- [ ] **Step 6: Commit**

Fold the design doc + this plan into this first commit (panni's rule: no separate commits for specs/plans):

```bash
cd /home/panni/xbmc
git add xbmc/guilib/guiinfo/GUIInfoLabels.h xbmc/GUIInfoManager.cpp xbmc/guilib/guiinfo/PlayerGUIInfo.cpp \
  docs/superpowers/specs/2026-06-08-codec-logos-active-area-design.md \
  docs/superpowers/plans/2026-06-08-codec-logos-active-area.md
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" \
  -m "DV: expose resolution-independent active-area aspect class to skins"
```

---

## Task 2: skin.plextuary — "Always keep codec logos inside the active area" setting

**Files:**
- Modify: `/mnt/d/data/___small/skin.plextuary/xml/SkinSettings.xml` (after the id=710 control, ~line 115)
- Modify: `/mnt/d/data/___small/skin.plextuary/language/resource.language.en_gb/strings.po`
- Modify: `/mnt/d/data/___small/skin.plextuary/language/resource.language.de_de/strings.po`

- [ ] **Step 1: Add the toggle control**

In `SkinSettings.xml`, immediately after the closing `</control>` of button id=710 (the line `</control>` at ~115, right before `<control type="radiobutton" id="908">`), insert:

```xml
				<control type="radiobutton" id="712">
					<label>$LOCALIZE[31280]</label>
					<include>DefaultSettingButton</include>
					<onclick>Skin.ToggleSetting(osd.codeclogosactiveareaoff)</onclick>
					<selected>!Skin.HasSetting(osd.codeclogosactiveareaoff)</selected>
				</control>
```

- [ ] **Step 2: Add the en_gb string**

In `language/resource.language.en_gb/strings.po`, after the `#31263` block ("Codec logos"), add:

```
msgctxt "#31280"
msgid "Always keep codec logos inside the active area"
msgstr ""
```

- [ ] **Step 3: Add the de_de string**

In `language/resource.language.de_de/strings.po`, add (place near the other codec-logo strings):

```
msgctxt "#31280"
msgid "Always keep codec logos inside the active area"
msgstr "Codec-Logos immer im aktiven Bildbereich halten"
```

- [ ] **Step 4: Verify well-formedness and presence**

Run:
```bash
xmllint --noout /mnt/d/data/___small/skin.plextuary/xml/SkinSettings.xml && echo "XML OK"
grep -n "osd.codeclogosactiveareaoff" /mnt/d/data/___small/skin.plextuary/xml/SkinSettings.xml
grep -n "31280" /mnt/d/data/___small/skin.plextuary/language/resource.language.en_gb/strings.po \
               /mnt/d/data/___small/skin.plextuary/language/resource.language.de_de/strings.po
```
Expected: "XML OK"; the setting id once; `31280` in both `.po` files.

- [ ] **Step 5: Commit**

```bash
cd /mnt/d/data/___small/skin.plextuary
git add xml/SkinSettings.xml language/resource.language.en_gb/strings.po language/resource.language.de_de/strings.po
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" \
  -m "Codec logos: add 'keep inside active area' setting (default on)"
```

---

## Task 3: skin.plextuary — active-area slide on the shared OSD/PPI includes

**Files:**
- Modify: `/mnt/d/data/___small/skin.plextuary/xml/Includes_CodecLogos.xml` (the three `CodecLogos*OSD` outer groups: `<left>35</left>` @7, `<left>855</left>` @301, `<left>1675</left>` @595)

The three includes are used by both the OSD (`Custom_1109_TopBarOverlay.xml`) and the PPI (`DialogPlayerProcessInfo.xml`), so editing the includes covers both render sites at once. Add the slides to the **outer** group (the one carrying the `Animation_FadeIn`/`FadeOut` includes and the `<left>` anchor), right after `Animation_FadeOut`.

- [ ] **Step 1: Left include (anchor `<left>35</left>`)**

Replace:
```xml
            <include>Animation_FadeOut</include>
            <left>35</left>
```
with:
```xml
            <include>Animation_FadeOut</include>
            <animation effect="slide" end="0,60" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),1)">Conditional</animation>
            <animation effect="slide" end="0,104" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),2)">Conditional</animation>
            <animation effect="slide" end="0,138" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),3)">Conditional</animation>
            <left>35</left>
```

- [ ] **Step 2: Center include (anchor `<left>855</left>`)**

Replace:
```xml
            <include>Animation_FadeOut</include>
            <left>855</left>
```
with the same three `<animation>` lines (identical conditions/ends) followed by `<left>855</left>`:
```xml
            <include>Animation_FadeOut</include>
            <animation effect="slide" end="0,60" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),1)">Conditional</animation>
            <animation effect="slide" end="0,104" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),2)">Conditional</animation>
            <animation effect="slide" end="0,138" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),3)">Conditional</animation>
            <left>855</left>
```

- [ ] **Step 3: Right include (anchor `<left>1675</left>`)**

Replace:
```xml
            <include>Animation_FadeOut</include>
            <left>1675</left>
```
with the same three `<animation>` lines followed by `<left>1675</left>`:
```xml
            <include>Animation_FadeOut</include>
            <animation effect="slide" end="0,60" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),1)">Conditional</animation>
            <animation effect="slide" end="0,104" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),2)">Conditional</animation>
            <animation effect="slide" end="0,138" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),3)">Conditional</animation>
            <left>1675</left>
```

- [ ] **Step 4: Verify well-formedness and count**

Run:
```bash
xmllint --noout /mnt/d/data/___small/skin.plextuary/xml/Includes_CodecLogos.xml && echo "XML OK"
grep -c "video.dovi.active.area.class" /mnt/d/data/___small/skin.plextuary/xml/Includes_CodecLogos.xml
```
Expected: "XML OK"; count `9` (3 classes × 3 includes).

- [ ] **Step 5: Commit**

```bash
cd /mnt/d/data/___small/skin.plextuary
git add xml/Includes_CodecLogos.xml
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" \
  -m "Codec logos: slide OSD/PPI logos into the DV active area by aspect class"
```

---

## Task 4: skin.p3i.estuary — same setting (default on)

**Files:**
- Modify: `/home/panni/skin.p3i.estuary/xml/SkinSettings.xml` (after the id=630 preview control, ~line 102)
- Modify: `/home/panni/skin.p3i.estuary/language/resource.language.en_gb/strings.po`
- Modify: `/home/panni/skin.p3i.estuary/language/resource.language.de_de/strings.po`

- [ ] **Step 1: Add the toggle control**

In `SkinSettings.xml`, immediately after the closing `</control>` of the preview control id=630, insert:

```xml
				<control type="radiobutton" id="632">
					<label>$LOCALIZE[31671]</label>
					<include content="DefaultSettingButton"><param name="textoffsetx">70</param></include>
					<onclick>Skin.ToggleSetting(osd.codeclogosactiveareaoff)</onclick>
					<selected>!Skin.HasSetting(osd.codeclogosactiveareaoff)</selected>
					<enable>!Skin.HasSetting(osd.hidecodecstartuplogos)</enable>
				</control>
```

- [ ] **Step 2: Add the en_gb string**

In `language/resource.language.en_gb/strings.po`, after the `#31670` block, add:

```
#: /xml/SkinSettings.xml
msgctxt "#31671"
msgid "Always keep codec logos inside the active area"
msgstr ""
```

- [ ] **Step 3: Add the de_de string**

In `language/resource.language.de_de/strings.po`, after the `#31670` block, add:

```
msgctxt "#31671"
msgid "Always keep codec logos inside the active area"
msgstr "Codec-Logos immer im aktiven Bildbereich halten"
```

- [ ] **Step 4: Verify well-formedness and presence**

Run:
```bash
xmllint --noout /home/panni/skin.p3i.estuary/xml/SkinSettings.xml && echo "XML OK"
grep -n "osd.codeclogosactiveareaoff" /home/panni/skin.p3i.estuary/xml/SkinSettings.xml
grep -n "31671" /home/panni/skin.p3i.estuary/language/resource.language.en_gb/strings.po \
               /home/panni/skin.p3i.estuary/language/resource.language.de_de/strings.po
```
Expected: "XML OK"; the setting id once; `31671` in both `.po` files.

- [ ] **Step 5: Commit**

```bash
cd /home/panni/skin.p3i.estuary
git add xml/SkinSettings.xml language/resource.language.en_gb/strings.po language/resource.language.de_de/strings.po
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" \
  -m "Codec logos: add 'keep inside active area' setting (default on)"
```

---

## Task 5: skin.p3i.estuary — active-area slide on the startup-flash groups

**Files:**
- Modify: `/home/panni/skin.p3i.estuary/xml/VideoFullScreen.xml` (group `id="2"` @76 and group `id="21"` @226)

Both groups already carry the L/C/R + horizontal position slides (`end="X,0"`). Add three vertical slides (`end="0,N"`) after the existing position slides in **each** group; they compose with the horizontal ones.

- [ ] **Step 1: Group id=2 — add vertical slides after the existing `82,0` position slide**

In group `id="2"`, after the line:
```xml
			<animation effect="slide" end="82,0" time="0" condition="Skin.HasSetting(osd.usecenterCodecLogos) + !Skin.HasSetting(osd.codeclogoshorizontal)">Conditional</animation>
```
insert:
```xml
			<animation effect="slide" end="0,60" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),1)">Conditional</animation>
			<animation effect="slide" end="0,104" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),2)">Conditional</animation>
			<animation effect="slide" end="0,138" time="0" condition="[ !Skin.HasSetting(osd.codeclogosactiveareaoff) | System.GetBool(coreelec.amlogic.dolbyvision.restrict.subs.active.area) ] + String.IsEqual(Player.Process(video.dovi.active.area.class),3)">Conditional</animation>
```

> Note: group id=2 and id=21 each contain exactly one `end="82,0"` position-slide line. Apply Step 1's insertion to the occurrence inside id=2 (the first, ~line 87) and Step 2's to the occurrence inside id=21 (the second, ~line 237). If using a text editor that needs unique anchors, include the surrounding group context to disambiguate.

- [ ] **Step 2: Group id=21 — add the same three vertical slides**

In group `id="21"`, after its `end="82,0"` position-slide line (the second occurrence in the file), insert the identical three `<animation>` lines from Step 1.

- [ ] **Step 3: Verify well-formedness and count**

Run:
```bash
xmllint --noout /home/panni/skin.p3i.estuary/xml/VideoFullScreen.xml && echo "XML OK"
grep -c "video.dovi.active.area.class" /home/panni/skin.p3i.estuary/xml/VideoFullScreen.xml
```
Expected: "XML OK"; count `6` (3 classes × 2 groups).

- [ ] **Step 4: Commit**

```bash
cd /home/panni/skin.p3i.estuary
git add xml/VideoFullScreen.xml
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" \
  -m "Codec logos: slide startup-flash logos into the DV active area by aspect class"
```

---

## Task 6: Version bumps + changelogs

**Files:**
- Modify: `/mnt/d/data/___small/skin.plextuary/addon.xml`, `/mnt/d/data/___small/skin.plextuary/changelog.txt`
- Modify: `/home/panni/skin.p3i.estuary/addon.xml`

- [ ] **Step 1: Plextuary version + changelog**

In `addon.xml`, bump `version="4.0.0-pm4k1.15cpm.a14.20"` → `version="4.0.0-pm4k1.15cpm.a14.21"`.

In `changelog.txt`, add at the very top:
```
[B]4.0.0-pm4k1.15cpm.a14.21[/B]
* Codec logos: option to keep them inside the DV active area (off the letterbox bars), default on (requires the corresponding p3i build for the active-area info)

```

- [ ] **Step 2: p3i version**

In `/home/panni/skin.p3i.estuary/addon.xml`, bump the version patch (current `8.8.11` per the publish workflow → `8.8.12`; confirm the actual current value first with `grep 'version=' addon.xml`).

- [ ] **Step 3: Verify**

Run:
```bash
grep -n 'version=' /mnt/d/data/___small/skin.plextuary/addon.xml | head -1
grep -n 'version=' /home/panni/skin.p3i.estuary/addon.xml | head -1
head -3 /mnt/d/data/___small/skin.plextuary/changelog.txt
```
Expected: bumped versions; new changelog entry at top.

- [ ] **Step 4: Commit (each repo)**

```bash
cd /mnt/d/data/___small/skin.plextuary
git add addon.xml changelog.txt
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" -m "Bump to a14.21"

cd /home/panni/skin.p3i.estuary
git add addon.xml
git -c user.email=1359593+pannal@users.noreply.github.com -c user.name=pannal \
  commit --author="pannal <1359593+pannal@users.noreply.github.com>" -m "Bump version for active-area codec-logo setting"
```

---

## Post-implementation (panni / manual — NOT part of automated execution)

- **No XBT repack needed:** all skin changes are XML/strings; no new or changed art.
- **Push:** panni pushes all three repos (Klaus never pushes).
- **p3i_repo publish** (panni's deploy, per `p3i-codec-logo-settings` memory): archive the skin into `p3i_repo/omega/skin.p3i.estuary`, run `python3 _repo_generator.py`, verify additive-only `addons.xml` diff, commit + push p3i_repo.
- **On-device A/B** per the spec's Testing section (both skins; 1080p AND 2160p DV at 2.40/2.0/16:9; default-on; opt-out; restriction-OR; p3i slide composition).

---

## Self-review

- **Spec coverage:** Component 1 (C++ class) → Task 1. Component 2 (gating + slides) → Tasks 3 (Plextuary OSD+PPI) & 5 (p3i startup flash). Component 3 (opt-out setting, default on) → Tasks 2 & 4. Version/changelog → Task 6. Testing/publish → Post-implementation. ✓
- **Placeholders:** none — every step has exact code/anchors. The one deferred lookup (p3i current `addon.xml` version) has an explicit `grep` to resolve it before editing. ✓
- **Type/name consistency:** `PLAYER_PROCESS_VIDEO_DOVI_ACTIVE_AREA_CLASS` / token `video.dovi.active.area.class` / setting `osd.codeclogosactiveareaoff` / strings `#31280` (Plextuary) & `#31671` (p3i) used consistently across tasks. Slide presets `0,60` / `0,104` / `0,138` for classes 1/2/3 identical everywhere. ✓
