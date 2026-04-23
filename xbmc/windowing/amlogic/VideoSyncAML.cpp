/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "ServiceBroker.h"
#include "windowing/GraphicContext.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
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
  const double fps = (m_fps > 1.0f) ? static_cast<double>(m_fps) : 60.0;
  const double frameIntervalUs = 1'000'000.0 / fps;
  const int64_t expectedIntervalNs = static_cast<int64_t>(1'000'000'000.0 / fps);

  // steady_clock so NTP wall-clock jumps don't corrupt elapsed math
  const auto startTs = std::chrono::steady_clock::now();
  uint64_t numVBlanks = 0;

  const auto legacyTimeout = std::chrono::microseconds(
      static_cast<int64_t>(std::max(8000.0, 3.0 * frameIntervalUs)));

  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  while (!stopEvent.Signaled() && !m_abort)
  {
    if (m_fbFd >= 0)
    {
      int64_t kernelTs = 0;
      if (ioctl(m_fbFd, FBIO_WAITFORVSYNC_64, &kernelTs) == 0)
      {
        if (kernelTs > 0 && kernelTs != m_lastKernelTs)
        {
          int countVSyncs = 1;
          if (m_lastKernelTs > 0 && expectedIntervalNs > 0)
          {
            const int64_t deltaNs = kernelTs - m_lastKernelTs;
            countVSyncs = static_cast<int>(
                (deltaNs + expectedIntervalNs / 2) / expectedIntervalNs);
            if (countVSyncs < 1)
              countVSyncs = 1;
          }
          m_lastKernelTs = kernelTs;
          numVBlanks += static_cast<uint64_t>(countVSyncs);
          m_refClock->UpdateClock(countVSyncs, CurrentHostCounter());
          continue;
        }
        // kernelTs == 0 when VD1 is powered down: drop to legacy path
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
