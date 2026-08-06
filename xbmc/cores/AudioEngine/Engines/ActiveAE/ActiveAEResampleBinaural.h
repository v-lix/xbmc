/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Interfaces/AEResample.h"
#include "cores/AudioEngine/Omniphony/OmniphonyLib.h"

#include <memory>
#include <vector>

namespace ActiveAE
{

/*!
 * \brief Converts multichannel audio to stereo by HRTF rendering.
 *
 * Kodi normally folds multichannel content to stereo with a matrix downmix,
 * which discards direction. This converter instead places every input channel
 * at a fixed position around the listener and renders it binaurally, so a
 * surround channel is heard from beside or behind rather than from inside the
 * head. The LFE bypasses the HRTF and is fed to both ears equally.
 *
 * It is selected in place of the stock resampler for exactly one shape of
 * conversion - multichannel in, stereo out - which is the conversion the
 * matrix downmix would otherwise have performed. Because the choice is made
 * from the conversion rather than from the output device, switching to a
 * multichannel sink deselects it with no setting to keep in sync.
 *
 * The class composes rather than replaces: it performs the channel conversion
 * and delegates rate conversion, sample format conversion, dithering and all
 * of AE's buffering arithmetic to an inner stock resampler, so those remain
 * exactly what they are today. If the engine cannot be loaded or starts
 * failing, it falls back to that inner resampler entirely and behaves like
 * unmodified Kodi.
 */
class CActiveAEResampleBinaural : public IAEResample
{
public:
  const char* GetName() override { return "ActiveAEResampleBinaural"; }

  CActiveAEResampleBinaural();
  ~CActiveAEResampleBinaural() override;

  /*!
   * \brief Whether this converter should handle a given conversion.
   *
   * True only for multichannel to stereo, in a source sample format this
   * class can interleave, and with the user setting enabled. Everything else
   * is left to the stock resampler.
   */
  static bool ShouldUse(const SampleConfig& dstConfig, const SampleConfig& srcConfig);

  bool Init(SampleConfig dstConfig,
            SampleConfig srcConfig,
            bool upmix,
            bool normalize,
            double centerMix,
            double surroundMix,
            CAEChannelInfo* remapLayout,
            AEQuality quality,
            bool force_resample,
            float sublevel) override;

  int Resample(uint8_t** dst_buffer,
               int dst_samples,
               uint8_t** src_buffer,
               int src_samples,
               double ratio) override;

  int64_t GetDelay(int64_t base) override;
  int GetBufferedSamples() override;
  bool WantsNewSamples(int samples) override;
  int CalcDstSampleCount(int src_samples, int dst_rate, int src_rate) override;
  int GetSrcBufferSize(int samples) override;
  int GetDstBufferSize(int samples) override;

private:
  //! \brief Whether a source sample format can be interleaved by Interleave().
  static bool IsSupportedSourceFormat(const SampleConfig& config);

  /*!
   * \brief Convert one input block to interleaved float in m_scratch.
   *
   * Deliberately hand-rolled rather than delegated to another resampler:
   * being stateless (n samples in, n samples out, never buffered) keeps the
   * sample accounting in Resample() exact.
   */
  void Interleave(uint8_t** src, int src_samples);

  //! \brief Give up on the engine and behave as the stock resampler from now on.
  void FallBack(const char* reason);

  const OmniphonyApi* m_api{nullptr};
  OrenderRenderer* m_renderer{nullptr};

  //! Stereo out of the engine, into whatever the sink wants. Always present.
  std::unique_ptr<IAEResample> m_inner;

  std::vector<float> m_scratch; //!< interleaved float input for the engine
  std::vector<float> m_render; //!< stereo float out of the engine

  SampleConfig m_src{};
  SampleConfig m_dst{};

  int m_consecutiveErrors{0};
  bool m_bypass{false}; //!< true once behaving exactly like the stock resampler
};

} // namespace ActiveAE
