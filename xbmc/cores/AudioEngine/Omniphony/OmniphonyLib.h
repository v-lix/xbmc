/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Omniphony/orender.h"

#include <string>

namespace ActiveAE
{

/*!
 * \brief Resolved entry points of the Omniphony spatial audio engine.
 *
 * liborender is loaded at runtime rather than linked, so Kodi builds without
 * it and an updated engine is picked up without rebuilding Kodi. Required
 * entries are never null once COmniphonyLib::Get() returns non-null; optional
 * entries are null when the loaded library predates them and MUST be checked
 * before use. Per the engine's ABI contract, optional features are gated on
 * symbol presence, never on the reported minor version.
 */
struct OmniphonyApi
{
  /* Required. A library missing any of these is rejected at load time. */
  OrenderRenderer* (*create)(const OrenderConfig* cfg);
  void (*destroy)(OrenderRenderer* r);
  int (*process)(OrenderRenderer* r,
                 const uint8_t* pkt,
                 uintptr_t pkt_len,
                 int64_t pts_us,
                 float* out,
                 uintptr_t out_cap_samples,
                 uintptr_t* out_frames,
                 uint32_t* out_channels,
                 int64_t* out_pts_us);
  void (*reset)(OrenderRenderer* r);
  uint32_t (*channel_count)(const OrenderRenderer* r);
  uint32_t (*channel_layout)(const OrenderRenderer* r, uint8_t* out_labels, uint32_t cap);
  int (*channel_mapping)(const OrenderRenderer* r);
  void (*set_channel_mode)(OrenderRenderer* r, int mode);

  /* Optional. Null when absent - always check before calling. */
  const char* (*build_id)();
  int (*set_option)(OrenderRenderer* r, const char* key, const char* value);

  /*!
   * \brief Render host-decoded PCM directly, bypassing the decoder bridge.
   *
   * Not part of ABI 0.6; probed so that when the engine gains it, the WAV
   * wrapper this integration currently needs can be dropped without touching
   * the loader. Null on every engine released so far, and nothing calls it.
   * The signature below is this integration's expectation, not a declaration
   * copied from orender.h - check it against the header that first ships the
   * symbol before calling through it.
   */
  int (*process_pcm)(OrenderRenderer* r,
                     const float* in,
                     uintptr_t in_frames,
                     uint32_t in_channels,
                     const uint8_t* labels,
                     uint32_t sample_rate,
                     int64_t pts_us,
                     float* out,
                     uintptr_t out_cap_samples,
                     uintptr_t* out_frames,
                     uint32_t* out_channels,
                     int64_t* out_pts_us);

  uint32_t abiMajor{0}; //!< as reported by the loaded library
  uint32_t abiMinor{0}; //!< diagnostics only; do not gate features on this
  std::string path; //!< candidate that loaded, for logging
};

/*!
 * \brief Runtime loader for liborender.
 *
 * The library is searched for, verified and loaded once per process; both
 * success and failure are cached, so a missing engine costs one search. The
 * handle is deliberately never dlclose()d: the engine keeps process-global
 * state and unloading it while any session exists would be unsafe.
 */
class COmniphonyLib
{
public:
  /*!
   * \brief Load the engine, or return the previously cached result.
   * \param explicitPath Optional absolute path overriding the search order.
   *        When set but unusable this is a hard failure with no fallback, so
   *        that a misconfigured path is visible rather than silently ignored.
   *        Read on the first call only: the load happens once per process, so
   *        a path supplied after the engine has been looked for is ignored.
   *        Nothing in the tree passes one today.
   * \return The resolved entry points, or nullptr when no compatible library
   *         was found. Callers must degrade gracefully; see GetError().
   */
  static const OmniphonyApi* Get(const std::string& explicitPath = "");

  /*!
   * \brief Human-readable reason the last load attempt failed.
   * \return Empty before the first Get() and after a successful load.
   */
  static const std::string& GetError();

private:
  static void Load(const std::string& explicitPath);
  static bool TryCandidate(const std::string& path);
};

} // namespace ActiveAE
