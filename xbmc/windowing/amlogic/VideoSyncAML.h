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
  // Stall detection on FBIO_WAITFORVSYNC_64. Counts consecutive "ts unchanged"
  // returns; when the count crosses a threshold we trigger one diagnostic dump
  // (via aml_dv_dump_state) so the blackout-class symptom — kernel stops
  // generating vsync after a mode switch — has captured state at the failure
  // point, not just at the prior DV transition. Latched so we don't spam.
  int m_staleTsCount{0};
  bool m_staleStallLogged{false};
};
