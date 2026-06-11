/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"

#include <atomic>
#include <memory>

class CVideoSync;

class CVideoReferenceClock : CThread, public ISettingCallback
{
  public:
    CVideoReferenceClock();
    ~CVideoReferenceClock() override;

    int64_t GetTime(bool interpolated = true);
    void    SetSpeed(double Speed);
    double  GetSpeed();
    double  GetRefreshRate(double* interval = nullptr);
    bool    GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const;

    void UpdateClock(int NrVBlanks, uint64_t time);
    void UpdateRefreshrate();

    // ISettingCallback — live-toggle the vsync ref clock when
    // coreelec.amlogic.usedisplayasclock is flipped from the GUI.
    void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

  private:
    void    Process() override;
    void Start();
    void Stop();
    void UpdateClockInternal(int NrVBlanks, bool CheckMissed);
    double  UpdateInterval() const;
    int64_t TimeOfNextVblank() const;

    int64_t m_CurrTime;          //the current time of the clock when using vblank as clock source
    int64_t m_LastIntTime;       //last interpolated clock value, to make sure the clock doesn't go backwards
    double  m_CurrTimeFract;     //fractional part that is lost due to rounding when updating the clock
    double  m_ClockSpeed;        //the frequency of the clock set by VideoPlayer
    int64_t m_SystemFrequency;   //frequency of the systemclock

    bool    m_UseVblank;         //set to true when vblank is used as clock source
    double  m_RefreshRate;       //current refreshrate
    int     m_MissedVblanks;     //number of clock updates missed by the vblank clock
    int     m_TotalMissedVblanks;//total number of clock updates missed, used by codec information screen
    int64_t m_VblankTime;        //last time the clock was updated when using vblank as clock

    CEvent m_vsyncStopEvent;
    // Set from OnSettingChanged when the user turns the toggle off; the
    // Process() loop checks this between iterations and exits cleanly,
    // letting IsRunning() flip to false so a subsequent Start() can spawn
    // a fresh thread.
    std::atomic<bool> m_disableRequested{false};

    mutable CCriticalSection m_CritSection;
    // Serialises Start()/Stop() against OnSettingChanged callbacks coming
    // from the settings thread so rapid toggling can't race the join.
    mutable CCriticalSection m_LifecycleSection;

    std::unique_ptr<CVideoSync> m_pVideoSync;
};
