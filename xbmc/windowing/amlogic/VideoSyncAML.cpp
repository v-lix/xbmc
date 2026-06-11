/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "ServiceBroker.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "windowing/GraphicContext.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "threads/Thread.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>

// Amlogic OSD-fb ioctl: blocks until next vsync, writes back s64 ktime (ns).
#ifndef FBIO_WAITFORVSYNC_64
#define FBIO_WAITFORVSYNC_64 _IOW('F', 0x21, unsigned int)
#endif

extern CEvent g_aml_sync_event;

CVideoSyncAML::CVideoSyncAML(CVideoReferenceClock *clock)
: CVideoSync(clock)
, m_abort(false)
{
}

CVideoSyncAML::~CVideoSyncAML()
{
}

bool CVideoSyncAML::Setup()
{
  m_abort = false;
  m_lastKernelTs = 0;
  // Reset all stall state so the fallback disengages on stop: every playback
  // (re-Setup) starts clean on the kernel vsync path.
  m_lastGoodTs = {};
  m_lastProbe = {};
  m_vsyncDegraded = false;
  m_failedProbes = 0;
  m_stallTs = 0;
  m_stallFaultLogged = false;
  m_legacyLatched = false;

  // Cache opt-in fallback setting once per Setup so the per-iteration hot
  // path doesn't touch the settings system. Re-evaluated on next playback.
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  m_fallbackOnStall = settings &&
    settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_VIDEOSYNC_FALLBACK_ON_STALL);

  CServiceBroker::GetWinSystem()->Register(this);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up AML");

  m_fbFd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (m_fbFd < 0)
  {
    CLog::Log(LOGINFO,
              "CVideoReferenceClock: unable to open /dev/fb0 for vsync ({}), "
              "falling back to legacy codec event",
              strerror(errno));
  }
  else
  {
    CLog::Log(LOGINFO, "CVideoReferenceClock: using FBIO_WAITFORVSYNC_64 on /dev/fb0");
  }

  return true;
}

void CVideoSyncAML::Run(CEvent& stopEvent)
{
  // steady_clock so NTP wall-clock jumps don't corrupt elapsed math
  auto startTs = std::chrono::steady_clock::now();
  uint64_t numVBlanks = 0;

  // Anchor the stall detector at Run start so a vsync that never produces a
  // single tick (sink that never syncs) still trips to legacy after the
  // threshold instead of blocking ~1s per ioctl forever; refreshed on every
  // advancing vsync below.
  m_lastGoodTs = startTs;

  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  // Default to 60Hz until the first valid GfxContext fps read.
  double last_fps = 0.0;
  double frameIntervalUs = 1'000'000.0 / 60.0;
  int64_t expectedIntervalNs = static_cast<int64_t>(1'000'000'000.0 / 60.0);
  auto legacyTimeout = std::chrono::microseconds(
      static_cast<int64_t>(std::max(8000.0, 3.0 * frameIntervalUs)));

  while (!stopEvent.Signaled() && !m_abort)
  {
    // Refresh fps each loop from GfxContext so display mode switches mid-Run
    // (e.g. GUI 60Hz → 1080p25 for PAL playback) take effect without needing
    // the framework to call OnResetDisplay. Otherwise expectedIntervalNs is
    // frozen at whatever fps the thread started with, and the kernel-ts
    // catch-up math interprets every real vsync as multiple vblanks.
    const double cur_fps = static_cast<double>(
        CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS());
    if (cur_fps != last_fps && cur_fps > 1.0)
    {
      frameIntervalUs = 1'000'000.0 / cur_fps;
      expectedIntervalNs = static_cast<int64_t>(1'000'000'000.0 / cur_fps);
      legacyTimeout = std::chrono::microseconds(
          static_cast<int64_t>(std::max(8000.0, 3.0 * frameIntervalUs)));
      // Reset the legacy-path baseline so its elapsedUs / frameIntervalUs
      // estimate starts fresh after a rate change.
      startTs = std::chrono::steady_clock::now();
      numVBlanks = 0;
      m_lastKernelTs = 0;
      // A mode switch is exactly when vsync briefly stalls then recovers; give
      // the kernel vsync path a clean retry instead of staying degraded, and
      // re-anchor the stall detector to now (startTs was just reset above) so
      // the settle after the switch is measured from here, not epoch-zero.
      m_lastGoodTs = startTs;
      m_vsyncDegraded = false;
      m_failedProbes = 0;
      m_stallFaultLogged = false;
      if (last_fps > 0.0)
      {
        CLog::Log(LOGDEBUG,
                  "CVideoSyncAML: fps changed {:.3f} → {:.3f}, reset clock",
                  last_fps, cur_fps);
        // Also refresh VideoReferenceClock::m_RefreshRate so UpdateInterval()
        // advances m_CurrTime by the correct ns-per-vblank for the new rate.
        // Without this, m_RefreshRate stays at whatever the rate was at
        // Setup time and m_CurrTime drifts; audio resampler corrects via
        // SetSpeed but the transition can produce visible micro-stutter.
        m_refClock->UpdateRefreshrate();
      }
      last_fps = cur_fps;
    }

    const auto nowSteady = std::chrono::steady_clock::now();
    const bool useVsyncPath = (m_fbFd >= 0) && !m_legacyLatched;

    if (useVsyncPath && !m_vsyncDegraded)
    {
      int64_t kernelTs = 0;
      if (ioctl(m_fbFd, FBIO_WAITFORVSYNC_64, &kernelTs) == 0)
      {
        if (kernelTs > 0 && kernelTs != m_lastKernelTs)
        {
          int countVSyncs = 1;
          int64_t deltaNs = 0;
          if (m_lastKernelTs > 0 && expectedIntervalNs > 0)
          {
            deltaNs = kernelTs - m_lastKernelTs;
            countVSyncs = static_cast<int>(
                (deltaNs + expectedIntervalNs / 2) / expectedIntervalNs);
            if (countVSyncs < 1)
              countVSyncs = 1;
          }
          if (countVSyncs > 1)
          {
            CLog::Log(LOGDEBUG,
                      "CVideoSyncAML: caught up {} vsyncs (deltaNs={}, "
                      "expectedNs={})",
                      countVSyncs, deltaNs, expectedIntervalNs);
          }
          m_lastKernelTs = kernelTs;
          m_lastGoodTs = nowSteady; // anchor for time-based stall detection
          numVBlanks += static_cast<uint64_t>(countVSyncs);
          m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());
          continue;
        }
        // kernelTs == 0 when VD1 is powered down (HDMI mode-switch settle,
        // VPP reconfig on seek, etc.); unchanged ts means the kernel ioctl
        // timed out (~1s) without a real vblank. Reset m_lastKernelTs so the
        // first valid ts after the gap is treated as a fresh anchor —
        // otherwise the catch-up math above reads deltaNs across the entire
        // blackout and advances m_CurrTime by N×interval in one shot, on top
        // of what the legacy fallback already advanced during the gap. That
        // jump is the dominant cause of audible TrueHD/MAT stutter + paired
        // video stall at start of playback and post-seek.
        if (kernelTs == 0)
          CLog::Log(LOGDEBUG, "CVideoSyncAML: ioctl returned ts=0 (VD1 off?), legacy fallback");
        else
          CLog::Log(LOGDEBUG, "CVideoSyncAML: ioctl ts unchanged ({}), legacy fallback", kernelTs);
        m_lastKernelTs = 0;

        // Time-based degrade trip. A brief gap (a single VPP reconfig on a
        // seek) clears within a frame or two and just falls through to legacy
        // for this iteration. Once vsync hasn't advanced for longer than the
        // threshold, riding the blocking ioctl means ~1s blocked per frame
        // while the clock lurches forward only once a second — the "black
        // picture, audio still playing" symptom — so drop to legacy timing and
        // stop issuing the blocking ioctl until a re-probe shows vsync is back.
        //
        // This is intentionally quiet: a legitimate multi-second HDMI mode
        // switch (AVR/projector trees relock in ~5s) looks identical to a stall
        // and gets the same treatment (ride legacy, auto-recover on relock).
        // Only a gap that outlasts any plausible mode switch is logged as a
        // fault below, in the re-probe branch.
        //
        // Floor at 750ms. In the dangerous case the kernel ioctl blocks its
        // full ~1s timeout before returning a stale ts, so the first stale
        // return already shows ~1s elapsed — any threshold up to ~1s trips on
        // it, and detection is bounded below by that kernel timeout regardless.
        // (6×interval only exceeds the floor below ~8fps.)
        const auto stallThreshold = std::chrono::microseconds(
            static_cast<int64_t>(std::max(750000.0, 6.0 * frameIntervalUs)));
        // Fresh timestamp: nowSteady was sampled at the loop top, before the
        // ioctl that just blocked up to ~1s, so it understates the gap. Measure
        // from after the blocking call so the first stale return trips at ~1s.
        const auto stnow = std::chrono::steady_clock::now();
        if ((stnow - m_lastGoodTs) > stallThreshold)
        {
          CLog::Log(LOGDEBUG,
                    "CVideoSyncAML: vsync gap {} ms — riding legacy timing, "
                    "re-probing for recovery",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        stnow - m_lastGoodTs).count());
          m_vsyncDegraded = true;
          m_failedProbes = 0;
          m_stallTs = kernelTs; // frozen ts; recovery = a probe ts beyond it
          m_lastProbe = stnow;
        }
        // Primary→legacy handoff: re-anchor the dead-reckoning ledger.
        // numVBlanks counted *real* vsyncs while the kernel path ran, but the
        // legacy estimator below counts nominal frame intervals against
        // startTs — the ppm-level difference between the display's actual
        // clock and the nominal rate (PLL tolerance, fractional-rate
        // approximation) diverges the two ledgers by a few frames per hour of
        // kernel-path runtime. Reconciling against a stale anchor would replay
        // all of that accumulated drift into the reference clock at once
        // (countVSyncs = curVBlanks - numVBlanks → one-shot jump, or repeated
        // zero-advances → freeze). Start dead-reckoning fresh from here
        // instead; the blocked-ioctl gap itself is already conserved by the
        // GetTime() synthesis + missed-vblank accounting.
        startTs = stnow;
        numVBlanks = 0;
        // fall through to legacy timing for this iteration
      }
      else
      {
        if (errno == EINTR)
          continue;
        if (errno == ENOTTY || errno == EINVAL)
        {
          CLog::Log(LOGINFO,
                    "CVideoReferenceClock: FBIO_WAITFORVSYNC_64 unsupported ({}), "
                    "permanently falling back to legacy path",
                    strerror(errno));
          close(m_fbFd);
          m_fbFd = -1;
          // Same ledger re-anchor as the stale-return handoff above: legacy
          // dead-reckoning owns the clock from here on. (In practice this
          // fires on the first ioctl of the session, so the anchor is fresh
          // anyway — kept for consistency.)
          startTs = std::chrono::steady_clock::now();
          numVBlanks = 0;
        }
      }
    }
    else if (useVsyncPath && m_vsyncDegraded)
    {
      // Degraded: ride legacy (pre-T4) wall-clock timing and re-probe the kernel
      // vsync at a bounded cadence. Legacy is open-loop dead-reckoning against
      // the *nominal* frame interval, so it drifts from the true display clock
      // the longer it runs — it is NOT a good steady state, it is a stopgap. Its
      // only merit over riding the stalled ioctl is that the reference clock
      // keeps advancing instead of freezing ~1s per blocking call, which keeps
      // audio/master-clock moving and makes recovery jump-free. It does NOT
      // repaint the display: if the gap is a true hardware plane stall the
      // picture stays black until vsync relocks regardless of clock pacing. So
      // the goal is to spend as little time here as possible — re-probe, and
      // snap back to the hardware vsync clock the instant a fresh advancing ts
      // returns (a recovered vsync answers immediately; a stalled one costs up
      // to ~1s, paid only while the stall persists).
      constexpr auto kProbeInterval = std::chrono::seconds(2);
      if (nowSteady - m_lastProbe >= kProbeInterval)
      {
        int64_t probeTs = 0;
        const bool ok = ioctl(m_fbFd, FBIO_WAITFORVSYNC_64, &probeTs) == 0;
        // Pace the next probe from when this (possibly 1s-blocking) one returns,
        // so a stalled probe doesn't immediately re-fire on the next iteration.
        m_lastProbe = std::chrono::steady_clock::now();
        if (ok && probeTs > 0 && probeTs > m_stallTs)
        {
          // Recovered — a mode switch relocked, or the stall cleared. If we had
          // escalated to a fault, note the recovery at the same level so the
          // log pairs up; otherwise this is the unremarkable mode-switch case.
          CLog::Log(m_stallFaultLogged ? LOGWARNING : LOGINFO,
                    "CVideoSyncAML: vsync recovered after {} ms — resuming "
                    "kernel vsync clock",
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        nowSteady - m_lastGoodTs).count());
          m_vsyncDegraded = false;
          m_failedProbes = 0;
          m_stallFaultLogged = false;
          m_lastKernelTs = 0;          // next normal-path ts is a fresh anchor
          m_lastGoodTs = m_lastProbe;
        }
        else
        {
          // Still stalled. Only now — once the gap has outlasted any plausible
          // HDMI mode switch (AVR/projector trees can take ~5s, sometimes more,
          // to relock) — treat it as a genuine fault and capture kernel state
          // once for diagnosis. The mitigation (legacy timing) already engaged
          // at 750ms, so erring high here only delays the log, never the fix.
          constexpr auto kFaultThreshold = std::chrono::seconds(10);
          if (!m_stallFaultLogged && (nowSteady - m_lastGoodTs) > kFaultThreshold)
          {
            CLog::Log(LOGWARNING,
                      "CVideoSyncAML: vsync stalled for {} ms (beyond any mode "
                      "switch) — capturing kernel state for diagnosis",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          nowSteady - m_lastGoodTs).count());
            aml_dv_dump_state("vsync_stall");
            m_stallFaultLogged = true;
          }

          if (m_fallbackOnStall && m_stallFaultLogged && ++m_failedProbes >= 3)
          {
            // Sustained, confirmed fault and the user opted into escalation:
            // stop paying the periodic re-probe hiccup and latch legacy timing
            // for the rest of this playback. Disengages on stop (Setup reset).
            CLog::Log(LOGWARNING,
                      "CVideoSyncAML: vsync stall sustained — latching legacy "
                      "timing for the rest of this playback");
            m_legacyLatched = true;
            CGUIDialogKaiToast::QueueNotification(
              CGUIDialogKaiToast::Warning,
              g_localizeStrings.Get(14307),
              "Switched to legacy timing",
              8000);
          }
        }
      }
      // fall through to legacy timing for this iteration
    }

    int countVSyncs = 1;
    if (!g_aml_sync_event.Wait(legacyTimeout))
    {
      const auto elapsed = std::chrono::steady_clock::now() - startTs;
      const double elapsedUs =
          std::chrono::duration<double, std::micro>(elapsed).count();

      const double expected = elapsedUs / frameIntervalUs;
      uint64_t curVBlanks = static_cast<uint64_t>(expected);

      const double nextBoundaryUs = (curVBlanks + 1) * frameIntervalUs;
      if (nextBoundaryUs > elapsedUs)
      {
        const int64_t sleepUs = static_cast<int64_t>(nextBoundaryUs - elapsedUs);
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        ++curVBlanks;
      }

      if (curVBlanks > numVBlanks)
      {
        countVSyncs = static_cast<int>(curVBlanks - numVBlanks);
        numVBlanks = curVBlanks;
      }
      else
      {
        countVSyncs = 0;
      }
    }
    else
    {
      ++numVBlanks;
    }

    if (countVSyncs > 0)
      m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());
  }
}

void CVideoSyncAML::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up AML");
  if (m_fbFd >= 0)
  {
    close(m_fbFd);
    m_fbFd = -1;
  }
  CServiceBroker::GetWinSystem()->Unregister(this);
}

float CVideoSyncAML::GetFps()
{
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: fps: {:.3f}", m_fps);
  return m_fps;
}

void CVideoSyncAML::OnResetDisplay()
{
  m_abort = true;
}
