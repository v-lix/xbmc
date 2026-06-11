/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/VideoSync.h"
#include "guilib/DispResource.h"

#include <chrono>
#include <cstdint>

class CVideoSyncAML : public CVideoSync, IDispResource
{
public:
  CVideoSyncAML(CVideoReferenceClock *clock);
  virtual ~CVideoSyncAML();
  virtual bool Setup()override;
  virtual void Run(CEvent& stopEvent)override;
  virtual void Cleanup()override;
  virtual float GetFps()override;
  virtual void OnResetDisplay()override;
private:
  volatile bool m_abort;
  int m_fbFd{-1};
  int64_t m_lastKernelTs{0};

  // Stall handling for FBIO_WAITFORVSYNC_64. The kernel ioctl is bounded
  // (wait_event_interruptible_timeout, ~1s) but on a vsync IRQ stall — VD1/VPP
  // reconfig on a seek, an HDMI mode switch, a wedged sink — it blocks the full
  // second and returns a *stale* timestamp. Riding that means ~1s blocked per
  // frame while the reference clock only lurches forward once a second: the
  // "black picture, audio still playing, recovers on stop" symptom.
  //
  // Instead, once vsync has not advanced for longer than the stall threshold
  // (a few frame intervals) we drop to legacy elapsed-time timing
  // (m_vsyncDegraded) and stop issuing the blocking ioctl every frame, then
  // re-probe at a bounded cadence; a single fresh advancing ts means vsync is
  // back and we resume the precise kernel clock. m_lastGoodTs is the wall-clock
  // time of the last advancing vsync; m_lastProbe paces the re-probe.
  std::chrono::steady_clock::time_point m_lastGoodTs{};
  std::chrono::steady_clock::time_point m_lastProbe{};
  bool m_vsyncDegraded{false};
  int m_failedProbes{0};
  // The frozen kernel timestamp captured when the stall tripped. ktime is
  // monotonic, so a re-probe returning a ts strictly greater than this means
  // the vsync IRQ has advanced again (recovery); equal means still stalled.
  int64_t m_stallTs{0};
  // Dropping to legacy after a short gap also covers a legitimate multi-second
  // HDMI mode switch (AVR/projector trees can take ~5s to relock), which looks
  // identical to a stall — vsync simply isn't ticking. So the short-gap drop is
  // quiet; only a gap that outlasts any plausible mode switch is logged as a
  // fault (WARNING + state dump). Latched so the dump fires once per episode.
  bool m_stallFaultLogged{false};

  // Escalation, opt-in via coreelec.amlogic.videosync.fallback.on.stall: when a
  // stall is sustained (not a transient seek glitch) latch legacy timing for
  // the rest of this playback rather than paying the periodic re-probe hiccup
  // on chronically-wedged hardware. All of the above — including this latch —
  // is reset in Setup(), so the fallback disengages on stop and every playback
  // starts clean on the kernel vsync path.
  bool m_legacyLatched{false};
  bool m_fallbackOnStall{false};
};
