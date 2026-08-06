/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ActiveAEResampleBinaural.h"

#include "ServiceBroker.h"
#include "cores/AudioEngine/AEResampleFactory.h"
#include "cores/AudioEngine/Omniphony/OmniphonyConfig.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <cstring>

extern "C"
{
#include <libavutil/samplefmt.h>
}

namespace ActiveAE
{

namespace
{

//! Output frames the engine may return for one input block before the buffer
//! is considered too small. Generous: the WAV bridge is close to 1:1, so this
//! is several times any plausible block.
constexpr size_t RENDER_CEILING_FRAMES = 8192;

//! Consecutive engine failures tolerated before giving up on it entirely. A
//! single bad block must not silence a whole track.
constexpr int MAX_CONSECUTIVE_ERRORS = 32;

/*!
 * \brief Build the 44-byte canonical WAVE header the decoder bridge expects.
 *
 * reference_bridge parses a RIFF/WAVE byte stream and derives the channel
 * labels from the channel count, so nothing beyond the standard header is
 * needed. The data length is left at its maximum: this is a stream, not a
 * file, and the bridge reads until it is reset.
 */
void BuildWavHeader(uint8_t* h, uint32_t rate, uint16_t channels)
{
  const uint32_t dataBytes = 0xFFFFFFFFu - 36u;
  const uint32_t riff = 36u + dataBytes;
  const uint32_t fmtSize = 16;
  const uint16_t formatTag = 3; // IEEE float
  const uint16_t bits = 32;
  const uint32_t byteRate = rate * channels * 4u;
  const uint16_t blockAlign = static_cast<uint16_t>(channels * 4);

  std::memcpy(h, "RIFF", 4);
  std::memcpy(h + 4, &riff, 4);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  std::memcpy(h + 16, &fmtSize, 4);
  std::memcpy(h + 20, &formatTag, 2);
  std::memcpy(h + 22, &channels, 2);
  std::memcpy(h + 24, &rate, 4);
  std::memcpy(h + 28, &byteRate, 4);
  std::memcpy(h + 32, &blockAlign, 2);
  std::memcpy(h + 34, &bits, 2);
  std::memcpy(h + 36, "data", 4);
  std::memcpy(h + 40, &dataBytes, 4);
}

} // unnamed namespace

CActiveAEResampleBinaural::CActiveAEResampleBinaural() = default;

CActiveAEResampleBinaural::~CActiveAEResampleBinaural()
{
  if (m_renderer && m_api)
    m_api->destroy(m_renderer);
}

bool CActiveAEResampleBinaural::IsSupportedSourceFormat(const SampleConfig& config)
{
  switch (config.fmt)
  {
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
      return true;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
      // Kodi also carries 24-bit audio in an int32, and whether the sample is
      // left- or right-justified depends on the format. Rather than guess,
      // only plain 32-bit is handled here; the rest falls to the stock
      // resampler, which already gets this right.
      return config.bits_per_sample == 32;
    default:
      return false;
  }
}

bool CActiveAEResampleBinaural::ShouldUse(const SampleConfig& dstConfig,
                                          const SampleConfig& srcConfig)
{
  // Only a genuine downmix qualifies. Asking about the conversion rather than
  // about the output device is what keeps this correct when the user switches
  // sinks: a multichannel destination simply never reaches this branch.
  if (srcConfig.channels <= 2 || dstConfig.channels != 2)
    return false;

  if (!IsSupportedSourceFormat(srcConfig))
    return false;

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings)
    return false;

  if (!settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_BINAURAL))
    return false;

  // Passthrough means an amplifier is doing the decoding, so the listener is on
  // speakers - no headphone DAC accepts a bitstream. The setting is hidden in
  // that case, and this keeps the hidden setting from staying in effect.
  // Deliberately mirrors how CActiveAE::LoadSettings resolves passthrough.
  return settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_CONFIG) == AE_CONFIG_FIXED ||
         !settings->GetBool(CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH);
}

void CActiveAEResampleBinaural::FallBack(const char* reason)
{
  if (m_bypass)
    return;

  CLog::Log(LOGWARNING, "Omniphony: {} - reverting to the standard downmix for this stream",
            reason);
  m_bypass = true;

  if (m_renderer && m_api)
  {
    m_api->destroy(m_renderer);
    m_renderer = nullptr;
  }
}

bool CActiveAEResampleBinaural::Init(SampleConfig dstConfig,
                                     SampleConfig srcConfig,
                                     bool upmix,
                                     bool normalize,
                                     double centerMix,
                                     double surroundMix,
                                     CAEChannelInfo* remapLayout,
                                     AEQuality quality,
                                     bool force_resample,
                                     float sublevel)
{
  m_src = srcConfig;
  m_dst = dstConfig;
  m_inner = CAEResampleFactory::Create();
  if (!m_inner)
    return false;

  // Anything that stops the engine coming up leaves this object behaving
  // exactly like the resampler it replaced - same Init arguments, same
  // result. That makes "engine missing" indistinguishable from stock Kodi.
  const std::string configPath = COmniphonyConfig::Resolve();
  m_api = configPath.empty() ? nullptr : COmniphonyLib::Get();

  if (!m_api)
  {
    if (!configPath.empty())
      CLog::Log(LOGWARNING, "Omniphony: {}", COmniphonyLib::GetError());
    m_bypass = true;
    return m_inner->Init(dstConfig, srcConfig, upmix, normalize, centerMix, surroundMix,
                         remapLayout, quality, force_resample, sublevel);
  }

  OrenderConfig cfg{};
  cfg.sample_rate = static_cast<uint32_t>(srcConfig.sample_rate);
  cfg.config_yaml_path = configPath.c_str();
  cfg.codec = "wav";
  m_renderer = m_api->create(&cfg);
  if (!m_renderer)
  {
    CLog::Log(LOGWARNING, "Omniphony: the engine could not start; check the bridge in {}",
              configPath);
    m_bypass = true;
    return m_inner->Init(dstConfig, srcConfig, upmix, normalize, centerMix, surroundMix,
                         remapLayout, quality, force_resample, sublevel);
  }

  // Channel-based content must be spatialised rather than handed back for the
  // host to deal with: 0 = host, 1 = spatial. Set explicitly so the result does
  // not depend on what the config happened to say.
  m_api->set_channel_mode(m_renderer, 1);

  const uint32_t outChannels = m_api->channel_count(m_renderer);
  if (outChannels != 2)
  {
    // Binaural output is stereo by construction. Anything else means the
    // engine is not configured the way this code assumes, and feeding its
    // output to a stereo-shaped inner resampler would be wrong.
    CLog::Log(LOGWARNING, "Omniphony: the engine returned {} output channels, expected 2",
              outChannels);
    m_api->destroy(m_renderer);
    m_renderer = nullptr;
    m_bypass = true;
    return m_inner->Init(dstConfig, srcConfig, upmix, normalize, centerMix, surroundMix,
                         remapLayout, quality, force_resample, sublevel);
  }

  // The engine renders at the source rate; the inner resampler carries that
  // stereo to whatever the sink actually wants. No upmix, no remap and no
  // centre/surround mixing: the render has already placed everything.
  SampleConfig mid = dstConfig;
  mid.fmt = AV_SAMPLE_FMT_FLT;
  mid.channels = 2;
  mid.sample_rate = srcConfig.sample_rate;
  mid.bits_per_sample = 32;
  mid.dither_bits = 0;
  CAEChannelInfo stereo;
  stereo = AE_CH_LAYOUT_2_0;
  mid.channel_layout = CAEUtil::GetAVChannelLayout(stereo);

  if (!m_inner->Init(dstConfig, mid, false, normalize, 1.0, 1.0, nullptr, quality, force_resample,
                     sublevel))
  {
    // The caller ignores this return value, as it does for the stock resampler,
    // so failing here must still leave an object that behaves. Bypass first,
    // then re-Init the inner resampler for the conversion it would have had if
    // this class had never been chosen.
    FallBack("the stereo stage could not be initialised");
    return m_inner->Init(dstConfig, srcConfig, upmix, normalize, centerMix, surroundMix,
                         remapLayout, quality, force_resample, sublevel);
  }

  m_render.resize(RENDER_CEILING_FRAMES * 2);

  // The bridge parses a WAVE stream and reset() returns it to expecting a
  // header, so one must precede the PCM. This runs on every Init(), and AE
  // recreates the resampler on every flush, which is what makes seeking work
  // without any special handling here.
  uint8_t header[44];
  BuildWavHeader(header, static_cast<uint32_t>(srcConfig.sample_rate),
                 static_cast<uint16_t>(srcConfig.channels));
  uintptr_t frames = 0;
  uint32_t channels = 0;
  int64_t pts = 0;
  if (m_api->process(m_renderer, header, sizeof(header), 0, m_render.data(), m_render.size(),
                     &frames, &channels, &pts) < 0)
  {
    FallBack("the engine rejected the stream header");
    return m_inner->Init(dstConfig, srcConfig, upmix, normalize, centerMix, surroundMix,
                         remapLayout, quality, force_resample, sublevel);
  }

  CLog::Log(LOGINFO, "Omniphony: binaural rendering {} channels at {} Hz to stereo",
            srcConfig.channels, srcConfig.sample_rate);
  return true;
}

void CActiveAEResampleBinaural::Interleave(uint8_t** src, int src_samples)
{
  const int channels = m_src.channels;
  const size_t total = static_cast<size_t>(src_samples) * channels;
  if (m_scratch.size() < total)
    m_scratch.resize(total);

  const bool planar = av_sample_fmt_is_planar(m_src.fmt) != 0;
  float* out = m_scratch.data();

  switch (m_src.fmt)
  {
    case AV_SAMPLE_FMT_FLT:
      std::memcpy(out, src[0], total * sizeof(float));
      break;

    case AV_SAMPLE_FMT_FLTP:
      for (int c = 0; c < channels; ++c)
      {
        const auto* in = reinterpret_cast<const float*>(src[c]);
        for (int s = 0; s < src_samples; ++s)
          out[s * channels + c] = in[s];
      }
      break;

    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
    {
      constexpr float scale = 1.0f / 32768.0f;
      for (int c = 0; c < channels; ++c)
      {
        const auto* in = reinterpret_cast<const int16_t*>(src[planar ? c : 0]);
        const int stride = planar ? 1 : channels;
        const int offset = planar ? 0 : c;
        for (int s = 0; s < src_samples; ++s)
          out[s * channels + c] = in[offset + s * stride] * scale;
      }
      break;
    }

    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
    {
      constexpr float scale = 1.0f / 2147483648.0f;
      for (int c = 0; c < channels; ++c)
      {
        const auto* in = reinterpret_cast<const int32_t*>(src[planar ? c : 0]);
        const int stride = planar ? 1 : channels;
        const int offset = planar ? 0 : c;
        for (int s = 0; s < src_samples; ++s)
          out[s * channels + c] = in[offset + s * stride] * scale;
      }
      break;
    }

    default:
      // Unreachable: ShouldUse() refuses anything else before we get here.
      std::memset(out, 0, total * sizeof(float));
      break;
  }
}

int CActiveAEResampleBinaural::Resample(
    uint8_t** dst_buffer, int dst_samples, uint8_t** src_buffer, int src_samples, double ratio)
{
  if (m_bypass)
    return m_inner->Resample(dst_buffer, dst_samples, src_buffer, src_samples, ratio);

  // A null source with no samples is AE draining the chain. There is nothing
  // to render; let the inner resampler flush what it still holds.
  if (src_buffer && src_samples > 0)
  {
    Interleave(src_buffer, src_samples);

    const size_t bytes = static_cast<size_t>(src_samples) * m_src.channels * sizeof(float);
    uintptr_t frames = 0;
    uint32_t channels = 0;
    int64_t pts = 0;
    const int ret =
        m_api->process(m_renderer, reinterpret_cast<const uint8_t*>(m_scratch.data()), bytes, 0,
                       m_render.data(), m_render.size(), &frames, &channels, &pts);

    if (ret > 0)
    {
      // Output would not fit and nothing was written. Grow for next time and
      // drop this block rather than re-feeding, which the engine does not
      // support. In practice this has never been observed to fire.
      m_render.resize(m_render.size() * 2);
      CLog::Log(LOGWARNING, "Omniphony: render buffer grown to {} floats, one block dropped",
                m_render.size());
      return 0;
    }

    if (ret < 0)
    {
      // A single failed block must not silence the track; only a sustained
      // run of them means the engine is really broken.
      if (++m_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS)
      {
        FallBack("the engine failed repeatedly");
        return m_inner->Resample(dst_buffer, dst_samples, src_buffer, src_samples, ratio);
      }
      return 0;
    }
    m_consecutiveErrors = 0;

    if (frames > 0)
    {
      if (channels != 2)
      {
        FallBack("the engine changed its output channel count mid-stream");
        return m_inner->Resample(dst_buffer, dst_samples, src_buffer, src_samples, ratio);
      }

      // The inner resampler consumes the whole block, buffering whatever does
      // not fit in dst_buffer; the remainder comes out on later calls.
      auto* rendered = reinterpret_cast<uint8_t*>(m_render.data());
      return m_inner->Resample(dst_buffer, dst_samples, &rendered, static_cast<int>(frames), ratio);
    }
  }

  // Nothing rendered this time: still give the inner resampler a chance to
  // emit anything it has buffered.
  return m_inner->Resample(dst_buffer, dst_samples, nullptr, 0, ratio);
}

int64_t CActiveAEResampleBinaural::GetDelay(int64_t base)
{
  return m_inner->GetDelay(base);
}

int CActiveAEResampleBinaural::GetBufferedSamples()
{
  return m_inner->GetBufferedSamples();
}

bool CActiveAEResampleBinaural::WantsNewSamples(int samples)
{
  return m_inner->WantsNewSamples(samples);
}

int CActiveAEResampleBinaural::CalcDstSampleCount(int src_samples, int dst_rate, int src_rate)
{
  return m_inner->CalcDstSampleCount(src_samples, dst_rate, src_rate);
}

int CActiveAEResampleBinaural::GetDstBufferSize(int samples)
{
  return m_inner->GetDstBufferSize(samples);
}

int CActiveAEResampleBinaural::GetSrcBufferSize(int samples)
{
  // Describes the true multichannel input, not the stereo stage in the
  // middle. Nothing in the tree currently calls this, but a future caller
  // must not be handed the wrong geometry.
  return av_samples_get_buffer_size(nullptr, m_src.channels, samples, m_src.fmt, 1);
}

} // namespace ActiveAE
