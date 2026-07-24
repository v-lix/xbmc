# Amlogic sysfs levers — p3i / T4 reference

Every `/sys` node our stack reads or writes, on both sides of the fence:

* **Kodi side** — `/mnt/d/data/___small/xbmc`, branch `aml-4.9-21.3_dev`, HEAD `ce0754a9a6`.
  All access goes through `CSysfsPath` (`xbmc/utils/SysfsPath.h`), which silently
  no-ops when a node is absent — that is why the same binary runs on non-Amlogic
  platforms.
* **Kernel side** — `/home/panni/linux-amlogic-local`, branch `amlogic-4.9-20`,
  HEAD `6d3761aa7b9f`.

## Provenance legend

| Tag | Meaning | How it was established |
|---|---|---|
| **p3i** | Added or re-defaulted on our kernel branch after the CPM base | present in `git diff e7a12df68cd3..HEAD` |
| **CPM** | Already there at the CPM base we forked from (`avdvplus/amlogic-4.9-20-cpm`, merge-base `e7a12df68cd3`) | absent from that diff, present in tree |
| **stock** | Amlogic / CoreELEC node, we only drive it | absent from that diff |

Caveat on **p3i**: the diff base is the CPM merge-base, so it also contains CoreELEC
upstream material merged in afterwards (the CEC class attributes and the `tsl2540`
ambient-light-sensor device attributes are of that kind, not our authorship). The
media/DV entries below are ours.

---

# 1. Kernel levers we add

## 1.1 `amdolby_vision` — the `xbmc_*` control channel

`/sys/module/amdolby_vision/parameters/<name>`, all mode `0664`. This is the
private ABI between Kodi and the DV kernel module: Kodi decides policy from
stream metadata plus user settings, the module applies it per frame. Nothing else
in the system writes these.

### Output pipeline

| Param | Type | Meaning |
|---|---|---|
| `xbmc_dv_type` | uint | Which DV signalling the sink gets: 0 display-led (DV-Std), 1/2 player-led HDR, 3 player-led LLDV. Derived from the *DV type* setting, then overridden by `xbmc_dv_vp`. |
| `xbmc_dv_vp` | uint | Video-processor mode 0–7. 0 = off; 1/2 = player-led HDR; 3/4/6 = LLDV YUV422; 5/7 = LLDV RGB444. 4/5 get bumped to 6/7 above 41 fps. Setting `…DV_VIDEO_PROCESSOR`. |
| `xbmc_dv_vp_tm` | uint | VP tone-map/bypass depth: >1 bypasses CVM in core1/core2, >2 bypasses CSC in core1, >3 forces core3 to IPT 12-bit bypass. Capped to 3 on profile 5 and to 2 for VP 5/7. |
| `xbmc_dv_profile` | uint | DV profile of the current stream (5, 7, 8.1 …), pushed by `aml_dv_send_profile()`. Read back by Kodi to cap `xbmc_dv_vp_tm`. |
| `xbmc_dv_el_type` | uint | Enhancement-layer type of the stream (MEL/FEL). |
| `xbmc_dv_non_ipt` | bool | Set for VS10 non-IPT output (HDR10/SDR). Tells the module to stop overriding HDMI colour params with DV-tunnel values, so a stale DV EOTF from a previous IPT mode does not stick. |
| `xbmc_dv_deep_color` | bool | Keep VPP internal precision at 12-bit (DAT_CONV + DOLBY_CTRL). For DV tunnel modes this never touches the wire, so the EDID 12-bit check is skipped; for VS10 non-IPT it is gated on the sink actually advertising 12-bit. Setting `…PREFER_12BIT`. |
| `xbmc_aml_linux_force_422` | bool | Force 4:2:2 on the wire during DV / HDR10+ playback. |

### Metadata injection

| Param | Type | Meaning |
|---|---|---|
| `xbmc_dv_md_source_max_pq` / `_min_pq` | ushort | Source mastering peak/black in PQ code, taken from the RPU. |
| `xbmc_dv_md_level_6_max_lum` / `_min_lum` / `_max_cll` / `_max_fall` | ushort | DV L6 static metadata from the stream. |
| `xbmc_dv_hdr10_max_lum` / `_min_lum` / `_max_cll` / `_max_fall` | ushort/uint | HDR10 static metadata for the non-DV case. |
| `xbmc_dv_hdr10_for_dv_ll` | bool | Emit an HDR10 InfoFrame alongside DV-LL output (player-led HDR / HDR2 only). |
| `xbmc_dv_hdr10_for_dv_ll_inject_num` | uint | How many frames to inject for; 0 = unlimited. Kodi always writes 0. |
| `xbmc_dv_vsvdb_inject` | bool | Replace the sink's VSVDB with our own payload. Setting `…DV_VSVDB_INJECT`. |
| `xbmc_dv_vsvdb_inject_num` | uint | Frame budget for the above; Kodi always writes 0. |
| `xbmc_dv_vsvdb_payload` | charp | Hex VSVDB bytes. User string, except in VP mode where Kodi forces `27FE012E5699AA` (vp_tm>1) or `27FE012D5699AA` (vp_tm==1). |
| `xbmc_dv_vsvdb_source_lum_limit_num` | uint | Frame budget for the source-luminance limit. Kodi always writes 0. |
| `xbmc_dv_hdr10plus_conv` | bool | **Dormant.** Kernel-side CMv4.0 injection for HDR10+→DV. The call site is commented out on both sides (`amdolby_vision.c:5865`, `AMLUtils.cpp:3875`); the parameter still exists. |

### Level 5 (active area / letterbox)

| Param | Type | Meaning |
|---|---|---|
| `xbmc_meta_level_5` | bool | Use the source's own L5 from the RPU. Setting `…DV_STD_SOURCE_LEVEL_5`, itself gated on `…DV_LEVEL5`. |
| `xbmc_meta_level_5_osdst` | bool | Extend that L5 to the OSD. |
| `xbmc_meta_level_5_subt` | bool | Extend it to subtitles. |
| `xbmc_detect_active_area` | bool | Master enable for the whole L5 substitution path. On when auto-detect, a `service.p3i.override` entry, or auto-letterbox needs it. |
| `xbmc_detected_l5_top` / `_bottom` / `_left` / `_right` | ushort | Bars measured by Kodi's active-area detector. Zeroed on stop and on detector reset. |
| `xbmc_override_l5_top` / `_bottom` / `_left` / `_right` | ushort | Explicit L5 values from a manual override or from auto-letterbox. |
| `xbmc_force_l5_override` | bool | Apply the override even when the frame carries a non-zero source L5. |
| `xbmc_l5_override_additive` | bool | **p3i, newest.** Add the override to the frame's source L5 instead of replacing it, so variable in-picture bars compose with player-added padding (3840×2024 with per-scene RPU L5 0/208 → emits 68/276). Auto-letterbox sets this; a manual override stays absolute. |

### VS10 → SDR

| Param | Type | Meaning |
|---|---|---|
| `xbmc_dv_sdr_keep_ext` | uint | Keep the source L1/L2 extension metadata on DV→SDR so the library does per-shot DDM with the colorist's SDR trims, rather than the static curve the avdvplus R8 strip leaves. DV sources only — HDR10 has no RPU to keep. Gates both the non-LL strip and the DV-LL (Player-Led) `in_scope` filter, which is the path SDR-display users actually run. Setting `…DV_VS10_SDR_PER_FRAME_METADATA`. |
| `xbmc_dv_sdr_src_max_nits` | uint | Cap the declared source peak the library tone-maps against (DV `source_max_pq` md bytes 66/67; HDR10 mastering peak, plus CLL/FALL in manual mode only), which lifts dim VS10 SDR output. 0 off, 1 auto (content MaxCLL), ≥100 manual nits. It is a **ceiling, not an override**: only ever lowers, and manual resolves to `min(content MaxCLL, N)` (`sdr_src_boost_nits()`), so values above the content's own peak collapse to auto. Content above the cap clips (HDR10) or soft-rolls off (DV). HDR10 is capped in a local `hdr10_param` copy, so the HDMI HDR infoframe keeps the real metadata; HLG is a no-op (library uses its nominal peak). SDR output only; the DV branch additionally requires VP off, the HDR10/HLG branch does not (it sits right before `control_path`, which is why `aml_dv_on` refreshes the param unconditionally). The `*→SDR` column of `dolby_vision_target_lum_max` is ignored by the library, so this is the working lever. Settings `…DV_VS10_SDR_BOOST` + `…DV_VS10_SDR_SRC_MAX_NITS`. |

## 1.2 `amdolby_vision` — stock params we re-default or drive

| Param | Provenance | Note |
|---|---|---|
| `dolby_vision_target_min` | **p3i** (`0664`) | Target display min luminance, 0.0001-nit units. Amlogic's 50 (0.005 nits) floors PQ black at 10-bit code ~77 → visible crush below ~80 (CE issue #83). We default it to 1 (code ~65). Setting `…DV_VS10_TARGET_MIN_LUM`. |
| `dv_HDR10_graphics_max` | **p3i** (`0664`) | Graphics peak used as `graphic_max` on the HDR10 output path (`amdolby_vision.c:6358`). Default 300. |
| `dolby_vision_mode` | CPM | Output mode: bypass / IPT / IPT tunnel / HDR10 / SDR10 / SDR8. |
| `dolby_vision_enable` | CPM | `Y`/`N` master switch. |
| `dolby_vision_policy` | CPM | Kodi pins it to force-output-mode, briefly to follow-source during teardown. |
| `dolby_vision_flags` | CPM | Bitfield; Kodi toggles `FLAG_FORCE_RGB_OUTPUT`, `FLAG_FORCE_DOVI_LL`, `FLAG_FORCE_CVM` (cleared for SDR output so `skip_cvm_tbl` can bypass CVM on SDR→SDR), and `FLAG_TOGGLE_FRAME` — asserted on output-mode changes, then polled until the kernel clears it (`aml_dv_toggle_frame()`). **p3i:** we force-clear it after a 3 s timeout (`0594cd12e5`); the kernel's consume path can't run before the first frame (`video_width/height` still 0), and a dangling request makes `FBIO_WAITFORVSYNC_64` return stale timestamps → "audio works, picture frozen". |
| `dolby_vision_ll_policy` | CPM | disable / YUV422 / RGB444. |
| `dolby_vision_graphic_max` | CPM | OSD brightness in nits. |
| `dv_graphic_blend_test` | CPM | OSD blend test path; Kodi clears it whenever it sets OSD brightness. |
| `dolby_vision_xbmc_osd` | CPM | Tells the module the Kodi OSD is up. |
| `dolby_vision_subtitles` | CPM | Tells the module subtitles are visible. |

`amdolby_vision` exports 88 module params in total: 41 `xbmc_*` (section 1.1), the 11
above, and 36 others (`dolby_vision_hdr10_policy`, `dolby_vision_use_source_meta_levels`,
`dolby_vision_target_lum_max`, `dv_ll_output_mode`, `force_mel`, `debug_dolby`, …) that
are stock and untouched by Kodi.

Two notes on `dolby_vision_target_lum_max` (the `[src][dst]` target-display peak matrix,
flattened to 9 values, `*→SDR` in slots 3/6/9). It is a `module_param_array`, so it takes
comma-separated values only — a space-separated write is rejected wholesale with `-EINVAL`.
More importantly, **the `*→SDR` column is inert**: the DV library ignores it, confirmed
on-device by A/B'ing 10 against 4000 on both DV→SDR and HDR10→SDR with identical results.
The vendor's own `FLAG_USE_SINK_MIN_MAX` block corroborates this: when the sink supplies a
target max it derives only the `→DOVI` and `→HDR` columns from EDID (`amdolby_vision.c:6400`),
never the `→SDR` one, and it is moot for us regardless — Kodi never sets that flag, so the
whole block, including its `dolby_vision_target_min` write, never runs. There is
therefore no way to tell the engine "tone-map to a 70-nit display"; `xbmc_dv_sdr_src_max_nits`
(section 1.1) is the working lever, capping the declared *source* peak instead.
`dolby_vision_target_min` is unaffected by any of this and is honoured normally — the
asymmetry is the library's, not ours.

## 1.3 `hdmitx20`

`/sys/module/hdmitx20/parameters/<name>`

| Param | Type/mode | Provenance | Meaning |
|---|---|---|---|
| `hdr10plus_vsif_hold` | bool `0644` | **p3i** | While set, `hdmitx_set_hdr10plus_pkt()` returns early on a normal (`flag == 1`) VSIF update instead of rewriting the packet, so the VSIF already on the wire survives a mode switch and the sink does not drop out of HDR10+. Explicit zero-VSIF and null-VSIF requests still go through. Kodi sets it before a mode change and clears it after (`WinSystemAmlogic.cpp:152/174`, `WinSystemAmlogicGLESContext.cpp:246`). |
| `dovi_tv_led_bt2020` | bool | CPM | Signal BT.2020 colorimetry in display-led DV. |
| `dovi_tv_led_no_colorimetry` | bool | CPM | Emit no colorimetry at all — needed for DV v2 sinks (`dv_ver_i == 2`). |
| `log_level`, `evaldata_verbose`, `max_exceed`, `hdmi_authenticated` | — | stock | Diagnostics; not driven by Kodi. |

## 1.4 Audio — `tdm`

`/sys/module/tdm/parameters/extra_pcm_layouts` — int `0644`, **p3i**, default 0.

Off, the driver advertises only the historical 2.0/3.1/5.1/7.1 channel maps and the
HDMI channel-allocation byte is byte-for-byte what it was. On, it additionally
advertises 4.0 (CA 0x08) and 5.0 (CA 0x0a), carried inside the 6-channel container
with the absent slots marked `SND_CHMAP_NA`, so AVRs report the real layout instead
of silent channels. Kodi writes it at stream open from
`SETTING_AUDIOOUTPUT_EXTRAPCMLAYOUTS` (`AESinkALSA.cpp:807`) — no reboot needed.
Auge SoCs only.

## 1.5 CEC — `hdmi_ao_cec`

`enable_cec_mailbox` — bool `0644`, **p3i**. Enables the SCPI mailbox sync in
`cec_save_mail_box`. Kernel-only; nothing in Kodi writes it.

The CEC class attributes exposed by that driver (`physical_addr`, `log_addr`,
`vendor_id`, `port_seq`, `menu_language`, `device_type`, `fun_cfg`, `dbg`, `dbg_en`,
`dump_status`) come from the CoreELEC CEC backport, not from us.

---

# 2. Kodi-side sysfs use

Every path below is reached through `CSysfsPath`. R = read, W = write.

## 2.1 Dolby Vision class device

| Node | R/W | Where | Purpose |
|---|---|---|---|
| `/sys/class/amdolby_vision/debug` | W | `AMLUtils.cpp:1452, 3003` | Command channel. Kodi writes `enable_fel 1` / `enable_fel 0` around FEL playback. |
| `/sys/class/amdolby_vision/dv_video_on` | R | `AMLUtils.cpp:351` | Polled by `aml_dv_wait_video_off()` during teardown. |
| `/sys/class/amdolby_vision/support_info` | R | `AMLUtils.cpp:710` | DV capability bitmask; DV counts as supported only when `(value & 7) == 7`. Probed once and cached. |
| `/sys/class/amdolby_vision/ko_info` | R | `AMLUtils.cpp:716` | Which DV library `.ko` is loaded. Read once, logged, otherwise unused. |

## 2.2 HDMI TX — `/sys/class/amhdmitx/amhdmitx0/`

| Node | R/W | Purpose |
|---|---|---|
| `attr` | R/W | Colour attribute string (`444,10bit`, `422,12bit`, …). Written to trigger `set_disp_mode_auto`. |
| `config` | R | Current HDMI configuration block; parsed for the player info OSD (`PlayerGUIInfo.cpp:76`) and for `aml_dv_dump_state`. |
| `disp_mode` | R | Kernel's current display mode; empty/`null` is part of the link-degraded signature. |
| `custom_mode` | R/W | Custom timing name for non-standard modes. |
| `frac_rate_policy` | R/W | 0 = integer rate, 1 = 1000/1001 fractional. Read back and re-applied on every mode set. |
| `phy` | W | Written 1 to bring the HDMI PHY back up. |
| `vid_mute` | W | Video mute, used to hide waste frames across transitions. |
| `stereo_mode` | W | 3D framepacking / SBS / TAB signalling. |
| `hpd_state`, `rhpd_state`, `rxsense_state`, `hdmi_used`, `sink_type` | R | Link state. `aml_hdmi_link_probe()` samples these at ~1 Hz from the GUI present hook and logs only on change; hpd/rhpd/rxsense at 0, or a lost `disp_mode`, is flagged as degraded — the "audio works, video frozen / blue screen" signature. |
| `edid` | R | Raw EDID; parsed for widescreen detection and DV capability. |
| `dv_cap` (also via `/sys/devices/virtual/amhdmitx/amhdmitx0/dv_cap`) | R | Sink DV block: version, LLDV/Std support, VSVDB. |
| `hdr_cap` | R | HDR10 / HLG / HDR10+ support. |
| `dc_cap` | R | Deep-colour support. `422,12bit` is what the Auto chroma path requires. |
| `disp_cap`, `disp_cap_3d`, `vesa_cap`, `allfmt_names`, `support_3d` | R | Mode enumeration. `disp_cap`/`disp_cap_3d` can be overridden from `special://home/userdata/disp_cap`, `disp_add`, `disp_cap_3d`; `vesa_cap` is only consulted when `/flash/vesa.enable` exists. |

## 2.3 Display and video planes

| Node | R/W | Purpose |
|---|---|---|
| `/sys/class/display/mode` | R/W | The active mode string. Also the target of the mode-set guard. |
| `/sys/class/display/axis` | R | Display geometry. |
| `/sys/class/video/axis` | W | Video plane rectangle. |
| `/sys/class/video/screen_mode` | W | 1 = normal, 4 = non-linear stretch. |
| `/sys/class/video/disable_video` | W | Hides the video plane. Logged explicitly — the gap between this write and a black screen is the primary diagnostic for the FEL "black with audio" class. |
| `/sys/class/video/freerun_mode` | W | Set to 1 when playing video disconnected from the audio clock. |
| `/sys/class/video/blackout_policy` | R/W | Whether the last frame is held or blanked on stop. |
| `/sys/class/video/fps_info` | R | Decoder-input / display-output fps pair, sampled into a 1-second history (`AMLUtils.cpp:3741`) and surfaced as the `PLAYER_PROCESS_AML_VIDEO_FPS_INFO` / `_FPS_DROP` info labels. |
| `/sys/class/graphics/fb0/free_scale`, `free_scale_axis`, `window_axis` | W | GUI free-scale. Sequence is: `free_scale=0`, set both axes, `free_scale=0x10001`. |
| `/sys/class/graphics/fb1/free_scale` | W | Turned off with fb0. |
| `/sys/class/graphics/fb{0,1}/blank` | W | Framebuffer blanking (`WinSystemAmlogic.cpp:350`, name is runtime-selected). |
| `/sys/class/graphics/fb0/debug` | W | OSD debug commands. |
| `/sys/class/ppmgr/ppscaler` | R | If the post-processing scaler is not enabled, Kodi sizes the video rect to the screen instead of the render resolution. |

## 2.4 Decoders, deinterlacer, sync

| Node | R/W | Purpose |
|---|---|---|
| `/sys/class/tsync/enable` | W | Disabled (0) for the duration of playback so Kodi owns A/V sync; restored to 1 on close so other apps still work. |
| `/sys/class/vfm/map` | R/W | Video-frame-manager pipeline map; read, edited, written back to insert/remove stages. |
| `/sys/class/deinterlace/di0/debug` | W | `di_debug_flag0x10000` forces interlaced handling as the VC1-progressive slow-framerate workaround (SoCs up to SC2 only, newer ones use ge2d copy); reset to `di_debug_flag0x0`. |
| `/sys/class/deinterlace/di0/frame_format` | R | Whether DI is actually in the path. Its presence also triggers a 12-field (~240 ms) start-timestamp shift so audio is delayed to match the DI pipeline. |
| `/sys/class/amstream/vcodec_profile` | R | Decoder capability enumeration. |
| `/sys/module/amvdec_h264/parameters/dec_control` | W | 4 = `DEC_CONTROL_FLAG_DISABLE_FAST_POC`. |
| `/sys/module/amvdec_h265/parameters/nal_skip_policy` | W | 1 for stream mode, 2 for frame mode. |
| `/sys/module/amvdec_h264mvc/parameters/view_mode` | W | 3D MVC view selection. |
| `/sys/module/amvideo/parameters/framepacking_support` | W | Enables 3D frame-packing output. |
| `/sys/module/di/parameters/nr2_en` | W | Noise reduction 2 disabled at startup (`WinSystemAmlogic.cpp:78`). |

## 2.5 Audio and memory

| Node | R/W | Purpose |
|---|---|---|
| `/sys/class/audiodsp/digital_raw` | W | 2 = passthrough, 0 = PCM. |
| `/sys/class/aml_ddr/urgent` | W | DMC port priority, `echo <port> <val>` with 1 non-urgent / 2 urgent / 4 super-urgent. We lift the DEVICE port to super-urgent so audio's ~3 MB/s requests are served alongside the display, which fixes the rare split-second HDMI audio dropout. The display pulls hundreds of MB/s and cannot be starved by audio. |

## 2.6 Platform / generic (not Amlogic-specific)

`/sys/bus/soc/devices/soc0/{machine,family,soc_id,revision,serial_number}`,
`/sys/devices/system/cpu/cpu0/cpufreq/{scaling_cur_freq,cpuinfo_max_freq}`,
`/sys/class/hwmon/hwmon*` — all read-only, `CPUInfoLinux.cpp` / `CPUInfoAndroid.cpp`.

## 2.7 debugfs

| Node | R/W | Purpose |
|---|---|---|
| `/sys/kernel/debug/amhdmitx/hdmi_pkt` | R | Polled to confirm the DV Std VSIF packet actually went out on the wire. |
| `/sys/kernel/debug/aml_reg/paddr` | R/W | Raw VPP/VPU register access behind `aml_read_reg()`. Debug only. |

---

# 3. Cross-cutting notes

**`aml_dv_dump_state(tag)`** (`AMLUtils.cpp:1277`) reads 46 of these nodes in one shot
and emits a single log line — the fastest way to see the whole DV + HDMI + geometry
state at a point in time. It is the reference list for "what matters" when triaging a
DV report.

**Teardown resets.** `aml_dv_off()` (`AMLUtils.cpp:1465–1487`) clears the full `xbmc_*`
channel plus the OSD/subtitle flags. Anything added to the channel needs a reset line
there, otherwise it leaks into the next title — that is the failure mode behind the
cross-title metadata leak in CE issue #85.

**Writers outside Kodi.** Kodi is not the only process on the box touching these nodes:
`service.p3i.override` pushes per-folder L5 values into the `xbmc_override_l5_*` /
`xbmc_force_l5_override` channel, and CoreELEC-Settings drives Amlogic display nodes of
its own. Their exact node sets were not audited for this document. The shared L5 channel
is visible in the Kodi code regardless — it is why `aml_dv_apply_l5_sysfs()` writes
`xbmc_detect_active_area` as a master enable ORing auto-detect, override and
auto-letterbox together, rather than mirroring a single setting.

**Absent nodes are not errors.** `CSysfsPath` treats a missing node as a no-op on write
and an empty optional on read, so a kernel without our patches degrades to stock
behaviour rather than failing.
