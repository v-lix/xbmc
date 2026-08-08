/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyConfig.h"

#include "OmniphonyHrtf.h"
#include "ServiceBroker.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <vector>

namespace ActiveAE
{

namespace
{

#if defined(TARGET_DARWIN)
constexpr const char* BRIDGE_NAME = "libreference_bridge.dylib";
#elif defined(TARGET_WINDOWS)
constexpr const char* BRIDGE_NAME = "reference_bridge.dll";
#else
constexpr const char* BRIDGE_NAME = "libreference_bridge.so";
#endif

constexpr const char* CONFIG_DIR = "special://temp/omniphony/";
constexpr const char* CONFIG_FILE = "special://temp/omniphony/config.yaml";

/*!
 * How much quieter the stock matrix downmix is when Kodi normalises it, for
 * 5.1 to stereo. Measured against the unnormalised fold of the same material:
 * 0.0999 against 0.2997 RMS, a factor of three.
 */
constexpr double NORMALIZED_DOWNMIX_DB = 9.5;

//! Room simulation setting values, in the order the setting lists them.
enum RoomPreset
{
  ROOM_OFF = 0,
  ROOM_SMALL = 1,
  ROOM_MEDIUM = 2,
  ROOM_LARGE = 3,
};

//! Shoebox dimensions in metres; a zero width means no room simulation.
struct Room
{
  double width;
  double depth;
  double height;
};

const Room& RoomFor(int preset)
{
  static const Room rooms[] = {
      {0.0, 0.0, 0.0}, // off
      {3.0, 3.5, 2.4}, // small
      {4.0, 5.0, 2.7}, // medium
      {6.0, 8.0, 3.2}, // large
  };
  if (preset < ROOM_OFF || preset > ROOM_LARGE)
    preset = ROOM_MEDIUM;
  return rooms[preset];
}

/*!
 * \brief Format a number for YAML, always with a decimal point.
 *
 * The engine reads these as floats; "3" would still parse, but a fixed-point
 * form reads consistently next to the values that really are fractional, and
 * two places is finer than any of these settings can be set to. Format() is
 * locale-independent, so the separator is a point wherever this runs.
 */
std::string Number(double value)
{
  return StringUtils::Format("{:.2f}", value);
}

/*!
 * \brief Quote a path for YAML.
 *
 * A user-chosen path can contain a colon, a leading digit or a quote, any of
 * which changes how a bare scalar is parsed. Double-quoted style with the two
 * escapes YAML defines for it is unambiguous for every path a file browser
 * can return.
 */
std::string Quote(std::string value)
{
  StringUtils::Replace(value, "\\", "\\\\");
  StringUtils::Replace(value, "\"", "\\\"");
  return "\"" + value + "\"";
}

//! \brief Read a whole file, or return an empty string when absent.
std::string ReadFile(const std::string& vfsPath)
{
  XFILE::CFile file;
  std::vector<uint8_t> data;
  if (!file.LoadFile(vfsPath, data))
    return {};
  return std::string(data.begin(), data.end());
}

} // unnamed namespace

std::string COmniphonyConfig::FindBridge()
{
  // Same locations the engine itself is searched for, so an addon or package
  // that ships both is found without any configuration.
  const std::vector<std::string> candidates = {
      CSpecialProtocol::TranslatePath("special://xbmcbin/omniphony/") + BRIDGE_NAME,
      CSpecialProtocol::TranslatePath("special://xbmc/system/omniphony/") + BRIDGE_NAME,
      CSpecialProtocol::TranslatePath("special://home/omniphony/") + BRIDGE_NAME,
  };

  for (const auto& candidate : candidates)
  {
    if (XFILE::CFile::Exists(candidate))
      return candidate;
  }
  return {};
}

std::string COmniphonyConfig::Emit(const std::string& bridgePath)
{
  // Kodi owns this file outright. The values not offered as settings are the
  // ones a listener cannot judge by ear in isolation - the head model, the
  // reverb time, the wall absorption - and are left at the tuned defaults.
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  double level = -3.0;
  double distance = 3.0;
  int room = ROOM_MEDIUM;
  int reverb = 25;
  const std::string sofa = COmniphonyHrtf::StagedPath();
  if (settings)
  {
    // Clamped to the ranges the settings declare rather than trusted: a
    // profile written before these existed reads every one of them as zero,
    // and a zero stage radius is not a quieter room, it is a degenerate one.
    level =
        std::clamp(settings->GetNumber(CSettings::SETTING_AUDIOOUTPUT_BINAURALLEVEL), -20.0, 10.0);
    distance =
        std::clamp(settings->GetNumber(CSettings::SETTING_AUDIOOUTPUT_BINAURALDISTANCE), 1.0, 6.0);
    room = std::clamp(settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_BINAURALROOM),
                      static_cast<int>(ROOM_OFF), static_cast<int>(ROOM_LARGE));
    reverb = std::clamp(settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_BINAURALREVERB), 0, 100);

    // "Binaural level" is relative to the fold this replaces, and that fold's
    // level depends on a setting of its own: with "maintain original volume"
    // off the matrix downmix is normalised, which for 5.1 to stereo is a
    // measured 9.5 dB quieter than the same fold unnormalised. Nothing of
    // that reaches the binaural path - the flag is passed to a stereo to
    // stereo inner resampler where it has nothing to do - so it is applied
    // here instead, and one number on the slider keeps one meaning whichever
    // way that setting is left. Exact for 5.1; other layouts normalise by a
    // slightly different amount and land within a decibel or so.
    if (!settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME))
      level -= NORMALIZED_DOWNMIX_DB;
  }

  std::string yaml;
  yaml += "# Generated by Kodi. Do not edit: this file is rewritten whenever\n";
  yaml += "# the settings that feed it change.\n";
  yaml += "render:\n";
  yaml += "  bridge_path: " + Quote(bridgePath) + "\n";
  yaml += "  master_gain: " + Number(level) + "\n";
  // Deliberately off. The engine's own clip protection is a one-way reduction
  // of the master gain that never comes back, so one loud transient would
  // quieten everything after it until the next session. Kodi limits this path
  // instead - CAELimiter attenuates on the peak, holds, then releases back to
  // unity - and the mixer now runs it for every binaural stream.
  yaml += "  auto_gain: false\n";
  yaml += "  binaural:\n";
  yaml += "    output_mode: binaural\n";

  // Already a checked local copy under the profile, or empty. Quoted anyway:
  // the profile directory is wherever the user put it, and a colon in that
  // path would otherwise split the scalar.
  if (sofa.empty())
  {
    yaml += "    hrir_source: saf\n";
  }
  else
  {
    yaml += "    hrir_source: sofa\n";
    yaml += "    hrtf_sofa_path: " + Quote(sofa) + "\n";
  }

  yaml += "    unit_scale_m: " + Number(distance) + "\n";
  yaml += "    head_radius_m: 0.0875\n";
  yaml += "    air_absorption: true\n";

  const Room& r = RoomFor(room);
  if (r.width <= 0.0)
  {
    yaml += "    reflections: { enabled: false }\n";
  }
  else
  {
    yaml += "    reflections: { enabled: true, room_width_m: " + Number(r.width) +
            ", room_depth_m: " + Number(r.depth) + ",\n";
    yaml += "                   room_height_m: " + Number(r.height) + ", level: 0.5 }\n";
  }

  if (reverb <= 0)
  {
    yaml += "    reverb: { enabled: false }\n";
  }
  else
  {
    yaml += "    reverb: { enabled: true, level: " + Number(reverb / 100.0) +
            ", rt60_s: 0.35, predelay_ms: 20 }\n";
  }

  return yaml;
}

std::string COmniphonyConfig::Resolve()
{
  const std::string bridge = FindBridge();
  if (bridge.empty())
  {
    CLog::Log(LOGWARNING,
              "Omniphony: no decoder bridge ({}) found; the engine cannot start without one",
              BRIDGE_NAME);
    return {};
  }

  const std::string wanted = Emit(bridge);
  const std::string path = CSpecialProtocol::TranslatePath(CONFIG_FILE);

  // Rewrite only on change. In steady state this is zero writes, which
  // matters on the flash storage these devices boot from.
  if (ReadFile(CONFIG_FILE) == wanted)
    return path;

  if (!XFILE::CDirectory::Exists(CONFIG_DIR) && !XFILE::CDirectory::Create(CONFIG_DIR))
  {
    CLog::Log(LOGERROR, "Omniphony: could not create {}", CONFIG_DIR);
    return {};
  }

  XFILE::CFile file;
  if (!file.OpenForWrite(CONFIG_FILE, true) ||
      file.Write(wanted.c_str(), wanted.size()) != static_cast<ssize_t>(wanted.size()))
  {
    CLog::Log(LOGERROR, "Omniphony: could not write {}", CONFIG_FILE);
    return {};
  }
  file.Close();

  CLog::Log(LOGINFO, "Omniphony: wrote {} (bridge {})", path, bridge);
  return path;
}

} // namespace ActiveAE
