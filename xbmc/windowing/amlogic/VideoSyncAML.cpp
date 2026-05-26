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
  m_staleTsCount = 0;
  m_staleStallLogged = false;

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
      m_staleTsCount = 0;
      m_staleStallLogged = false;
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

    if (m_fbFd >= 0)
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
          numVBlanks += static_cast<uint64_t>(countVSyncs);
          m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());
          // Fresh valid ts → recover from any stall state.
          m_staleTsCount = 0;
          m_staleStallLogged = false;
          continue;
        }
        // kernelTs == 0 when VD1 is powered down (HDMI mode-switch settle,
        // VPP reconfig on seek, etc.); unchanged ts means the wake fired
        // without a real vblank. Either way drop to legacy. Reset
        // m_lastKernelTs so the first valid ts after the gap is treated as
        // a fresh anchor — otherwise the catch-up math above reads deltaNs
        // across the entire blackout and advances m_CurrTime by N×interval
        // in one shot, on top of what the legacy fallback already advanced
        // during the gap. That jump is what AE samples right when playback
        // engages after a refresh-rate-change delay, and is the dominant
        // cause of audible TrueHD/MAT stutter + paired video stall at
        // start of playback and post-seek.
        if (kernelTs == 0)
          CLog::Log(LOGDEBUG, "CVideoSyncAML: ioctl returned ts=0 (VD1 off?), legacy fallback");
        else
          CLog::Log(LOGDEBUG, "CVideoSyncAML: ioctl ts unchanged ({}), legacy fallback", kernelTs);
        m_lastKernelTs = 0;
        // Stall detection: count consecutive non-progressing returns. Short
        // bursts during mode switches / VPP reconfig are normal — only log if
        // the stall sustains. Threshold ~24 iterations is ~1s at 24Hz; far
        // above any legitimate transient (mode-switch settle is ~100-300ms).
        // Latched so we only dump once per stall episode.
        ++m_staleTsCount;
        if (!m_staleStallLogged && m_staleTsCount >= 24)
        {
          CLog::Log(LOGWARNING,
                    "CVideoSyncAML: vsync stalled ({} consecutive stale ioctl "
                    "returns) — capturing kernel state for diagnosis",
                    m_staleTsCount);
          aml_dv_dump_state("vsync_stall");
          m_staleStallLogged = true;

          if (m_fallbackOnStall)
          {
            // Permanent (per-session) fallback to legacy timing. Close the
            // fb to make the next loop iterations skip the ioctl entirely.
            // Reset on next Setup() so a fresh playback gets another shot
            // at the kernel vsync path.
            CLog::Log(LOGWARNING,
                      "CVideoSyncAML: stall-fallback enabled — closing "
                      "/dev/fb0 and staying on legacy timing for this session");
            close(m_fbFd);
            m_fbFd = -1;
            CGUIDialogKaiToast::QueueNotification(
              CGUIDialogKaiToast::Warning,
              g_localizeStrings.Get(14307),
              "Switched to legacy timing",
              8000);
          }
        }
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
        }
      }
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
