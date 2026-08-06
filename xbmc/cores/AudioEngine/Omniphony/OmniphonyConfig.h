/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

namespace ActiveAE
{

/*!
 * \brief Supplies the engine's configuration file.
 *
 * The engine is configured by a YAML file handed to orender_create(). Kodi
 * either owns that file completely or does not touch it at all - there is no
 * merging, so no YAML parser is needed here, only string emission.
 */
class COmniphonyConfig
{
public:
  /*!
   * \brief Resolve the config path to pass to orender_create().
   *
   * Emits Kodi's own configuration to a file under special://temp (tmpfs on
   * CoreELEC; these boxes run from flash and a write per playback is
   * avoidable wear) and returns its translated path. The file is rewritten
   * only when its content actually changes.
   *
   * \return An absolute path, or an empty string when the config could not be
   *         written or no decoder bridge could be found.
   */
  static std::string Resolve();

  /*!
   * \brief Locate the PCM decoder bridge the engine needs.
   *
   * The engine cannot start without a bridge. For channel-based rendering
   * that is reference_bridge, which presents a multichannel WAV byte stream
   * as a channel bed.
   *
   * \return An absolute path, or an empty string when none was found.
   */
  static std::string FindBridge();

private:
  static std::string Emit(const std::string& bridgePath);
};

} // namespace ActiveAE
