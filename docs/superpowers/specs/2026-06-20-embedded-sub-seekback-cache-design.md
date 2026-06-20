# Embedded text-subtitle seek-back recall (hybrid cache + secondary demuxer)

Date: 2026-06-20
Status: design approved (panni), pre-implementation
Scope: Kodi (`/home/panni/xbmc`, branch `aml-4.9-21.3_dev`)

## Problem

Embedded text subtitles (ASS/SSA/SRT/mov_text/WebVTT) disappear after a seek-back
until the next subtitle event — sometimes for a long time, especially with large
GOP. Reported by nissel (log akujesepab; AM6B p3i T4a).

Root cause (code-traced): a subtitle event active at the seek target `T` is a
point-block muxed at its start `S ≤ T`. After a seek:

- `matroska_read_seek` repositions the byte stream to the **video** keyframe
  cluster `≤ T`; subtitle tracks are exempt from the keyframe skip-gate, so ffmpeg
  delivers every subtitle block it *reads past* — but a block muxed earlier than
  the landing cluster is never read.
- `CDVDDemuxFFmpeg::SeekTime` sets `startpts = requested time T`; for accurate
  seeks `CVideoPlayer::CheckPlayerInit` drops any packet with `dts < startpts`, so
  even a re-read active subtitle (dts `S < T`) is dropped.

Net: the currently-active subtitle is never re-emitted. Gap length = time to the
next subtitle event after `T`.

Prior art: the merged stream-switch fix (`c3d3d877b8` + `ffc8fc4005`, xbmc#27770)
only fires on subtitle *stream change*, not on plain seeks. An earlier rolling-
window packet cache failed for **PGS image subs** on BD — not relevant here since
this is text-only and a full (not windowed) cache.

The render-cache invalidation commit `62c3d3f02a` is a separate, already-confirmed
bug (pts-keyed libass render cache surviving a seek). It stays; it is unrelated to
this fix.

## Approach: hybrid

Two layers behind one feature:

1. **Fast path — incremental packet cache.** During playback, cache the selected
   embedded *text* subtitle stream's packets. On seek, reinject the event(s)
   active at `T` directly into the subtitle player. Free (zero extra I/O); covers
   continuous-playback seek-back (the dominant case).

2. **Fallback — secondary subtitle-only demuxer.** On a cache *miss* (seek into a
   region never demuxed, e.g. resume mid-file then seek back before the resume
   point), open/seek a second demuxer on the same file, read the subtitle stream
   around `T`, reinject the active event(s), and fold them into the cache. Small
   I/O, only on miss. Also covers forward-seek into unplayed content (bonus).

Both layers feed the same **reinject** primitive, which bypasses
`CheckPlayerInit` (so the `dts < startpts` drop does not kill the recalled event).

## Components (all in `CVideoPlayer`)

### 1. Reinject primitive
`void ReinjectSubtitlePackets(const std::vector<DemuxPacket*>& pkts)`

- For each source packet, allocate a **copy** via `CDVDDemuxUtils::AllocateDemuxPacket(iSize)`,
  `memcpy` `pData`, and copy `pts`, `dts`, `duration`, `iStreamId`, `demuxerId`,
  `iGroupId`, `subtitlePlane`, `m_ptsOffsetCorrection`. (Text subs carry no crypto
  and effectively no side data; side data/crypto are not copied. Assert/skip if
  present.)
- Wrap each copy in `std::make_shared<CDVDMsgDemuxerPacket>(copy, false)` and
  `m_VideoPlayerSubtitle->SendMessage(...)`. `drop=false` → the overlay codec
  decodes it and adds the overlay. This path does **not** go through
  `ProcessSubData`/`CheckPlayerInit`.
- Ownership: the player retains the cache's originals; the message owns its copy.
  No double-free (the failure mode flagged in the old windowed impl).
- Precondition: subtitle stream open (`m_pOverlayCodec` exists). After a seek the
  subtitle stream is still open (flush does not close it), so this holds.

Display: for SSA the packet feeds a libass event `[S, S+dur]`; libass renders it
at clock `T` if active. For Text/TX3G/WebVTT the overlay carries `[S, E]` and the
container shows it while the clock is in range. Reinject happens **after**
`FlushBuffers` (which clears stale overlays), so the recalled event is the only one
re-added.

### 2. Incremental cache
- Storage: `std::map<double, DemuxPacket*> m_subtitleSeekCache`, keyed by packet
  `pts` (corrected timeline; see Timestamps). Map → automatic dedup of the
  re-reads that follow a seek-back.
- Coverage: `std::vector<std::pair<double,double>> m_subtitleSeekCovered` — merged
  `[from,to]` pts ranges actually demuxed, used to distinguish a genuine "no
  subtitle active here" gap from never-read territory. Inserting a packet at `pts`
  extends/merges the current range by `[pts, pts+max(duration,0)]`.
- `m_subtitleSeekCacheStreamId` — the stream the cache belongs to. If the selected
  subtitle stream changes, clear cache + coverage and reset.
- Memory bound: cap total cached bytes at a generous constant (`32 MiB`). On
  exceed: `log()` once and stop caching new packets (degrade gracefully; never hit
  for text — a whole-movie ASS track is single-digit MB). Do not evict (eviction
  reintroduces the window hole).
- Fill hook: in `ProcessSubData`, before the `m_VideoPlayerSubtitle->SendMessage`
  drop logic, if the stream is the selected embedded (`STREAM_SOURCE_DEMUX`) text
  subtitle (codec gate below): cache a copy keyed by `pts`, extend coverage.
- Clear: on file open (`PLAYER_OPENFILE`), close, and subtitle-stream switch.
  **Never** on flush/seek.

### 2b. Re-read duplicate suppression
`CDVDOverlayCodecSSA::Decode` renumbers each event's libass ReadOrder from its own
counter, so libass cannot dedup a reinjected event against the demuxer's own
re-read of it. On an accurate seek `CheckPlayerInit` drops the re-read (dts <
startpts), but on a keyframe-accurate (fast) seek — default on — it does not, so
the event would be added twice → doubled overlay. Mitigation: `ReinjectSubtitlePackets`
records each re-emitted event's pts in `m_subtitleReinjectedPts`; `ProcessSubData`
drops (from delivery only — still cached) the first demuxed packet whose pts
matches, then forgets it. Gated on the cache belonging to the current stream so it
cannot false-match on a different (e.g. image) subtitle. Cleared each seek and on
cache clear.

### 3. Secondary subtitle-only demuxer
- `std::shared_ptr<CDVDDemuxFFmpeg> m_pSubtitleCatchupDemuxer` +
  `std::shared_ptr<CDVDInputStream>` on the same file, created **lazily** on first
  cache-miss (pattern: `AddSubtitleFile` `.sup` branch, VideoPlayer.cpp ~5222):
  `CreateInputStream(nullptr, m_item)` → `Open()` → `CDVDDemuxFFmpeg::Open(pInput, false)`.
  Own input stream = own file position; seeking it cannot disturb main playback.
- `std::vector<DemuxPacket*> FetchActiveSubtitleFromFile(double T, int streamId)`:
  - Same physical file → same stream indices; target the same subtitle stream id.
  - `SeekTime(T - CATCHUP_WINDOW, true /*backward*/)` on the secondary demuxer
    (`CATCHUP_WINDOW = 60s`; backward seek lands on the video keyframe ≤ that, so
    effective reach is ≥ window).
  - `Read()` forward, keeping only packets of `streamId`; apply
    `UpdateCorrection(pkt, m_offset_pts)` (secondary packets are raw); collect
    those with `pts ≤ T < pts + duration`. Stop once `pts > T`.
  - Also insert collected packets into `m_subtitleSeekCache` + extend coverage, so
    nearby subsequent seeks become fast-path hits.
  - Cap the read loop (packet count / time budget) so a pathological file cannot
    spin.
- Lifecycle: created lazily, kept alive for the file, reset on file open/close.

### 4. Text-vs-image codec gate
Text (cache + recall): `AV_CODEC_ID_TEXT`, `AV_CODEC_ID_SUBRIP`, `AV_CODEC_ID_SSA`,
`AV_CODEC_ID_ASS`, `AV_CODEC_ID_MOV_TEXT`, `AV_CODEC_ID_WEBVTT`.
Excluded (unchanged behavior): PGS/DVB/DVD/XSUB (image subs → `CDVDOverlayCodecFFmpeg`).
Implement as a small `static bool IsCachableTextSubtitle(AVCodecID)`.

### 5. Seek hook
In `HandleMessages`, `PLAYER_SEEK` case, **after** `FlushBuffers(start, ...)`:
- Gate: current subtitle is embedded text and visible; feature enabled.
- `T` = seek target in the cache/clock domain (same value used for `startpts`,
  `DVD_MSEC_TO_TIME(time)`). Verify the domain matches cache keys with a debug log
  during bring-up.
- `active = FindActive(T)` from the cache (events whose `[pts, pts+duration)`
  contains `T`).
- If `active` non-empty → `ReinjectSubtitlePackets(active)`.
- Else if `T` lies outside all covered ranges (cache-miss) →
  `ReinjectSubtitlePackets(FetchActiveSubtitleFromFile(T, streamId))`.
- Else (inside covered range, no active event) → genuine gap, do nothing.

Threading: `ProcessSubData` (fill) and `HandleMessages` (reinject/fetch) both run on
`CVideoPlayer::Process()` — single thread, **no locking**.

## Timestamps
Cached packets are captured in `ProcessSubData`, i.e. after `ReadPacket` applied
`UpdateCorrection(pkt, m_offset_pts)` — they are on the corrected/clock timeline,
which matches the post-seek clock and `startpts`. `m_offset_pts` is **not** reset on
embedded-subtitle seeks (the reset is gated to external subs, `a21b606a20`), so the
basis is stable across seeks. Secondary-demuxer packets are **raw**, so
`FetchActiveSubtitleFromFile` applies `UpdateCorrection(pkt, m_offset_pts)` before
reinject/caching.

## Gating
The cache + reinject **fast path is always on** — it is free (zero extra I/O) and
safe on any storage, so there is no reason to gate it.

Only the **cache-miss file-read fallback** (the second demuxer) is gated, by
`coreelec.subtitles.recallfromfile` (boolean, label/help #60612/#60613), **default
OFF**. That path competes for I/O and, despite the cache pre-check + wall-clock
budget, can briefly disturb playback on slow disk/USB/network — so it is opt-in.
The member `m_subtitleSeekRecallFromFile` is read from the setting in
`OpenInputStream()`.

## Non-goals / known limitations
- **PGS / image subs**: out of scope; unchanged.
- **External subtitles**: unaffected (separate demuxer + file parser already handle
  seeks).
- **Event starting before a covered range, active within it** (e.g. a 2-minute sign
  spanning the very start of a covered interval): the fast path may miss it; the
  secondary reader is only triggered for targets *outside* coverage. Rare; documented.
- **Forward-seek into unplayed content**: handled by the secondary reader as a
  side effect, not a primary goal.
- **Recall needs the cache bound to the current stream first** (set on the first
  cached packet). An immediate seek-back in the brief window after a fresh resume,
  before any subtitle packet has been demuxed, won't recall; it self-heals on the
  next seek. Conservative on purpose — avoids mixing streams in the cache.

## Testing / verification
No subtitle unit-test harness exists in-tree. Verification = on-device A/B (panni):
1. Continuous playback, repeated seek-back into sparse dialogue → active sub
   reappears immediately (fast path).
2. Resume mid-file, seek back before the resume point → active sub appears
   (secondary reader).
3. Large-GOP file from the report → fixed.
4. PGS/image-sub file → behavior unchanged.
5. Watch for: no double-free/leak on close; no playback hitch beyond a brief,
   bounded pause on the rare secondary-fetch.

## Files (anticipated)
- `xbmc/cores/VideoPlayer/VideoPlayer.h` — members + method decls.
- `xbmc/cores/VideoPlayer/VideoPlayer.cpp` — cache, reinject, secondary fetch,
  `ProcessSubData` fill hook, `PLAYER_SEEK` recall hook, lifecycle clears, gate.
- `xbmc/settings/...` + strings — the toggle (if kept).
