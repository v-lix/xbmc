/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "VideoReferenceClock.h"

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "interfaces/AnnouncementManager.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "utils/Variant.h"
#include "windowing/GraphicContext.h"
#include "windowing/VideoSync.h"
#include "windowing/WinSystem.h"

#include <mutex>

namespace
{
// "Is something CDVDClock-relevant playing right now."
//
// IsPlayingVideo() (= IsPlaying && HasVideo) races against the VideoPlayer
// processing thread setting m_HasVideo in OpenVideoStream(): at OnPlay
// dispatch, m_HasVideo is still false for a legitimate video file, so we'd
// skip Start and only catch up at OnAVStart, leaving CDVDClock on the wall
// clock during stream sync. Inverting the check — "skip only if no player
// or a definitively audio-only player" — closes that window because at
// OnPlay both m_HasVideo and m_HasAudio are false, so IsPlayingAudio() is
// false and we Start immediately. PAPlayer hardcodes HasAudio() = true, so
// PAPlayer audio still correctly resolves to skip. The only false positive
// is an audio-only file routed through VideoPlayer (rare); OnAVStart's
// re-evaluation catches that and Stops the briefly-started clock.
bool ShouldClockRun()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (settings && !settings->GetBool(
          CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK))
    return false;

  const auto player =
      CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (!player || !player->IsPlaying())
    return false;
  if (player->IsPlayingAudio())
    return false;
  return true;
}
} // namespace

CVideoReferenceClock::CVideoReferenceClock() : CThread("RefClock")
{
  m_SystemFrequency = CurrentHostFrequency();
  m_ClockSpeed = 1.0;
  m_TotalMissedVblanks = 0;
  m_UseVblank = false;

  m_CurrTime = 0;
  m_LastIntTime = 0;
  m_CurrTimeFract = 0.0;
  m_RefreshRate = 0.0;
  m_MissedVblanks = 0;
  m_VblankTime = 0;
  m_vsyncStopEvent.Reset();

  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->RegisterCallback(this,
          {CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK});
  }

  if (const auto announcer = CServiceBroker::GetAnnouncementManager())
    announcer->AddAnnouncer(this);

  // Thread no longer auto-starts at construction; the OnPlay announcer
  // spawns it on playback start (if the setting is on), and OnStop joins
  // it. This avoids burning ~24-60 ioctls/sec on /dev/fb0 while idle.
}

CVideoReferenceClock::~CVideoReferenceClock()
{
  if (const auto announcer = CServiceBroker::GetAnnouncementManager())
    announcer->RemoveAnnouncer(this);

  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->UnregisterCallback(this);
  }

  m_bStop = true;
  m_vsyncStopEvent.Set();
  StopThread();
}

void CVideoReferenceClock::Start()
{
  std::unique_lock<CCriticalSection> lock(m_LifecycleSection);

  if (IsRunning())
    return;

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (settings && !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK))
    return;

  // Reset transient state so a re-spawn after Stop() starts cleanly.
  m_disableRequested = false;
  m_vsyncStopEvent.Reset();
  m_bStop = false;

  Create();
}

void CVideoReferenceClock::Stop()
{
  std::unique_lock<CCriticalSection> lock(m_LifecycleSection);

  if (!IsRunning())
    return;

  // Signal both gates: m_disableRequested makes the outer Process() loop
  // break instead of re-entering Setup(); m_vsyncStopEvent unblocks the
  // CVideoSync::Run() that is currently parked on a vsync wait.
  m_disableRequested = true;
  m_vsyncStopEvent.Set();
  StopThread(true);
  // Leave m_bStop as StopThread left it; Start() will clear it before
  // the next Create().
}

void CVideoReferenceClock::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting)
    return;

  if (setting->GetId() != CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK)
    return;

  CLog::Log(LOGINFO, "CVideoReferenceClock: vsync ref-clock setting toggled");
  ReevaluateState();
}

void CVideoReferenceClock::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                                    const std::string& sender,
                                    const std::string& message,
                                    const CVariant& data)
{
  if (flag != ANNOUNCEMENT::Player)
    return;

  // OnPlay/OnAVStart/OnStop all re-evaluate desired state against the
  // ShouldClockRun() gate. Subscribing to all three closes various race
  // windows: OnPlay catches the start before HasVideo is even set
  // (ShouldClockRun() inverts to "skip only audio-only"); OnAVStart
  // re-checks once HasVideo/HasAudio are definitive (corrects a false
  // positive for audio-only-via-VideoPlayer); OnStop tears down only when
  // nothing video-relevant is playing — for a file-to-file swap on the
  // same CVideoPlayer the new file is already IsPlaying when the old
  // file's OnStop is finally dispatched, so the clock survives the swap.
  if (message == "OnPlay" || message == "OnAVStart" || message == "OnStop")
    ReevaluateState();
}

void CVideoReferenceClock::ReevaluateState()
{
  if (ShouldClockRun())
    Start();
  else
    Stop();
}

void CVideoReferenceClock::UpdateClock(int NrVBlanks, uint64_t time)
{
  std::unique_lock<CCriticalSection> lock(m_CritSection);

  m_VblankTime = time;
  UpdateClockInternal(NrVBlanks, true);
}

void CVideoReferenceClock::Process()
{
  bool SetupSuccess = false;
  int64_t Now;

  while(!m_bStop)
  {
    m_pVideoSync = CServiceBroker::GetWinSystem()->GetVideoSync(this);

    if (m_pVideoSync)
    {
      SetupSuccess = m_pVideoSync->Setup();
      UpdateRefreshrate();
    }

    std::unique_lock<CCriticalSection> SingleLock(m_CritSection);
    Now = CurrentHostCounter();
    m_CurrTime = Now;
    m_LastIntTime = m_CurrTime;
    m_CurrTimeFract = 0.0;
    m_ClockSpeed = 1.0;
    m_TotalMissedVblanks = 0;
    m_MissedVblanks = 0;

    if (SetupSuccess)
    {
      m_UseVblank = true;          //tell other threads we're using vblank as clock
      m_VblankTime = Now;          //initialize the timestamp of the last vblank
      SingleLock.unlock();

      // we might got signalled while we did not wait
      if (!m_vsyncStopEvent.Signaled())
      {
        //run the clock
        m_pVideoSync->Run(m_vsyncStopEvent);
        m_vsyncStopEvent.Reset();
      }
    }
    else
    {
      SingleLock.unlock();
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setup failed, falling back to CurrentHostCounter()");
    }

    SingleLock.lock();
    m_UseVblank = false;                       //we're back to using the systemclock
    SingleLock.unlock();

    //clean up the vblank clock
    if (m_pVideoSync)
    {
      m_pVideoSync->Cleanup();
      m_pVideoSync.reset();
    }

    if (!SetupSuccess)
      break;

    // Honour a live-toggle disable: the user flipped
    // coreelec.amlogic.usedisplayasclock to false; exit cleanly so the
    // thread joins and IsRunning() becomes false. Start() will re-spawn
    // a fresh thread on the next enable.
    if (m_disableRequested.exchange(false))
    {
      CLog::Log(LOGINFO,
                "CVideoReferenceClock: vsync ref-clock disabled — exiting thread");
      break;
    }
  }
}

//this is called from the vblank run function and from CVideoReferenceClock::Wait in case of a late update
void CVideoReferenceClock::UpdateClockInternal(int NrVBlanks, bool CheckMissed)
{
  if (CheckMissed) //set to true from the vblank run function, set to false from Wait and GetTime
  {
    if (NrVBlanks < m_MissedVblanks) //if this is true the vblank detection in the run function is wrong
      CLog::Log(
          LOGDEBUG,
          "CVideoReferenceClock: detected {} vblanks, missed {}, refreshrate might have changed",
          NrVBlanks, m_MissedVblanks);

    const int origNr = NrVBlanks;
    const int origMissed = m_MissedVblanks;
    NrVBlanks -= m_MissedVblanks; //subtract the vblanks we missed
    m_MissedVblanks = 0;
    if (NrVBlanks <= 0)
    {
      // real vsync arrived but synthesized vblanks already ate the budget;
      // m_CurrTime won't advance on this real tick.
      CLog::Log(LOGDEBUG,
                "CVideoReferenceClock: UpdateClock skipped advance "
                "(NrVBlanks={} - missed={} = {})",
                origNr, origMissed, NrVBlanks);
    }
  }
  else
  {
    m_MissedVblanks += NrVBlanks;      //tell the vblank clock how many vblanks it missed
    m_TotalMissedVblanks += NrVBlanks; //for the codec information screen
    // double divide so fractional rates (23.976/29.97/59.94) don't round up to integer
    m_VblankTime += static_cast<int64_t>(
        static_cast<double>(m_SystemFrequency) / m_RefreshRate * NrVBlanks);
  }

  if (NrVBlanks > 0) //update the clock with the adjusted frequency if we have any vblanks
  {
    double increment = UpdateInterval() * NrVBlanks;
    double integer   = floor(increment);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5); //make sure it gets correctly converted to int

    //accumulate what we lost due to rounding in m_CurrTimeFract, then add the integer part of that to m_CurrTime
    m_CurrTimeFract += increment - integer;
    integer          = floor(m_CurrTimeFract);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5);
    m_CurrTimeFract -= integer;
  }
}

double CVideoReferenceClock::UpdateInterval() const
{
  return m_ClockSpeed / m_RefreshRate * static_cast<double>(m_SystemFrequency);
}

//called from dvdclock to get the time
int64_t CVideoReferenceClock::GetTime(bool interpolated /* = true*/)
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);

  //when using vblank, get the time from that, otherwise use the systemclock
  if (m_UseVblank)
  {
    int64_t  NextVblank;
    int64_t  Now;

    Now = CurrentHostCounter();        //get current system time
    NextVblank = TimeOfNextVblank();   //get time when the next vblank should happen

    int synth = 0;
    const int64_t stale_us = (Now > m_VblankTime)
        ? (Now - m_VblankTime) * 1'000'000 / m_SystemFrequency
        : 0;
    while(Now >= NextVblank)  //keep looping until the next vblank is in the future
    {
      UpdateClockInternal(1, false); //update clock when next vblank should have happened already
      NextVblank = TimeOfNextVblank(); //get time when the next vblank should happen
      ++synth;
    }
    if (synth > 1)
    {
      CLog::Log(LOGDEBUG,
                "CVideoReferenceClock: GetTime synthesis fired {} iterations "
                "(stale_us={}, missed={}, total_missed={})",
                synth, stale_us, m_MissedVblanks, m_TotalMissedVblanks);
    }

    if (interpolated)
    {
      //interpolate from the last time the clock was updated
      double elapsed = static_cast<double>(Now - m_VblankTime) * m_ClockSpeed;
      //don't interpolate more than 2 vblank periods
      elapsed = std::min(elapsed, UpdateInterval() * 2.0);

      //make sure the clock doesn't go backwards
      int64_t intTime = m_CurrTime + static_cast<int64_t>(elapsed);
      if (intTime > m_LastIntTime)
        m_LastIntTime = intTime;

      return m_LastIntTime;
    }
    else
    {
      return m_CurrTime;
    }
  }
  else
  {
    return CurrentHostCounter();
  }
}

void CVideoReferenceClock::SetSpeed(double Speed)
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);
  //VideoPlayer can change the speed to fit the rereshrate
  if (m_UseVblank)
  {
    if (Speed != m_ClockSpeed)
    {
      m_ClockSpeed = Speed;
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Clock speed {:0.2f} %", m_ClockSpeed * 100.0);
    }
  }
}

double CVideoReferenceClock::GetSpeed()
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);

  //VideoPlayer needs to know the speed for the resampler
  if (m_UseVblank)
    return m_ClockSpeed;
  else
    return 1.0;
}

void CVideoReferenceClock::UpdateRefreshrate()
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);
  m_RefreshRate = static_cast<double>(m_pVideoSync->GetFps());
  m_ClockSpeed = 1.0;

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: {:.3f} hertz", m_RefreshRate);
}

//VideoPlayer needs to know the refreshrate for matching the fps of the video playing to it
double CVideoReferenceClock::GetRefreshRate(double* interval /*= NULL*/)
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    if (interval)
      *interval = m_ClockSpeed / m_RefreshRate;

    return m_RefreshRate;
  }
  else
    return -1;
}

#define MAXVBLANKDELAY 13LL
//guess when the next vblank should happen,
//based on the refreshrate and when the previous one happened
//increase that by 30% to allow for errors
int64_t CVideoReferenceClock::TimeOfNextVblank() const
{
  // double divide for fractional-rate precision; consistent with UpdateClockInternal
  return m_VblankTime + static_cast<int64_t>(
      static_cast<double>(m_SystemFrequency) / m_RefreshRate * MAXVBLANKDELAY / 10.0);
}

//for the codec information screen
bool CVideoReferenceClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const
{
  std::unique_lock<CCriticalSection> SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    MissedVblanks = m_TotalMissedVblanks;
    ClockSpeed = m_ClockSpeed;
    RefreshRate = m_RefreshRate;
    return true;
  }
  return false;
}
