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

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class CSetting;
class CAEStreamInfo;
struct IntegerSettingOption;
struct StringSettingOption;

namespace ActiveAE
{
class CActiveAE;

class CActiveAESettings : public ISettingCallback
{
public:
  CActiveAESettings(CActiveAE &ae);
  ~CActiveAESettings() override;

  /*!
   * \brief Vet a personal HRTF file before it is accepted.
   *
   * The only setting this refuses. A SOFA file is opaque to the person
   * choosing it - the extension says nothing about whether the engine can use
   * what is inside - and the engine's reader will take a file it half
   * understands and render noise rather than failing. So the file is copied
   * into the profile and checked here, and a selection that does not survive
   * that is rejected outright with the reason on screen, leaving whatever was
   * chosen before still in place.
   */
  bool OnSettingChanging(const std::shared_ptr<const CSetting>& setting) override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

  /*!
   * \brief Keep the HRTF mode and the file it names describing the same thing.
   *
   * Choosing the built-in set discards the staged copy and empties the file
   * control; choosing a personal one with nothing staged asks for the file
   * there and then, and falls back to built-in if none is given.
   */
  static void OnHrtfModeChanged(int mode);

  static void SettingOptionsAudioDevicesFiller(const std::shared_ptr<const CSetting>& setting,
                                               std::vector<StringSettingOption>& list,
                                               std::string& current,
                                               void* data);
  static void SettingOptionsAudioDevicesPassthroughFiller(
      const std::shared_ptr<const CSetting>& setting,
      std::vector<StringSettingOption>& list,
      std::string& current,
      void* data);
  static void SettingOptionsAudioQualityLevelsFiller(const std::shared_ptr<const CSetting>& setting,
                                                     std::vector<IntegerSettingOption>& list,
                                                     int& current,
                                                     void* data);
  static void SettingOptionsAudioStreamsilenceFiller(const std::shared_ptr<const CSetting>& setting,
                                                     std::vector<IntegerSettingOption>& list,
                                                     int& current,
                                                     void* data);
  static void SettingOptionsBluetoothCodecsFiller(const std::shared_ptr<const CSetting>& setting,
                                                   std::vector<StringSettingOption>& list,
                                                   std::string& current,
                                                   void* data);
  static bool IsSettingVisible(const std::string& condition,
                               const std::string& value,
                               const std::shared_ptr<const CSetting>& setting,
                               void* data);

protected:
  static void SettingOptionsAudioDevicesFillerGeneral(
      const std::shared_ptr<const CSetting>& setting,
      std::vector<StringSettingOption>& list,
      std::string& current,
      bool passthrough);

  CActiveAE &m_audioEngine;
  CCriticalSection m_cs;
  static CActiveAESettings* m_instance;
};
};
