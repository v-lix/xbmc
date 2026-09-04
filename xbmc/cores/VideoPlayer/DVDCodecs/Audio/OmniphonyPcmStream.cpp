/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyPcmStream.h"

extern "C"
{
#include <libavutil/channel_layout.h>
}

namespace
{
//! \brief The label values the renderer's bridge_api defines for fixed speaker
//! positions. Kept as literals rather than an enum of our own: they cross a
//! process and a language boundary, so the numbers themselves are the contract.
enum OmniphonyLabel : uint8_t
{
  OMNI_L = 0,
  OMNI_R = 1,
  OMNI_C = 2,
  OMNI_LFE = 3,
  OMNI_LS = 4, //!< side left
  OMNI_RS = 5, //!< side right
  OMNI_TFL = 6,
  OMNI_TFR = 7,
  OMNI_TSL = 8,
  OMNI_TSR = 9,
  OMNI_TBL = 10,
  OMNI_TBR = 11,
  OMNI_LSC = 12, //!< front left of centre
  OMNI_RSC = 13, //!< front right of centre
  OMNI_LB = 14, //!< back left
  OMNI_RB = 15, //!< back right
  OMNI_CB = 16, //!< back centre
  OMNI_TC = 17,
  OMNI_LSD = 18, //!< surround direct left
  OMNI_RSD = 19, //!< surround direct right
  OMNI_LW = 20, //!< wide left
  OMNI_RW = 21, //!< wide right
  OMNI_TFC = 22,
  OMNI_LFE2 = 23,
};

/*!
 * \brief One FFmpeg channel bit and the renderer position it is.
 *
 * Every label the renderer defines for a fixed speaker is reachable from a
 * mask bit, so the only unmappable positions are FFmpeg's own: TOP_BACK_CENTER,
 * which the renderer does not name; STEREO_LEFT/RIGHT, which mark a downmix
 * rather than a speaker; and the BOTTOM_* family. A stream carrying one of
 * those falls back to ordinary decoding rather than being placed approximately.
 */
struct LabelMapping
{
  uint64_t bit;
  uint8_t label;
};

constexpr LabelMapping MAPPINGS[] = {
    {AV_CH_FRONT_LEFT, OMNI_L},
    {AV_CH_FRONT_RIGHT, OMNI_R},
    {AV_CH_FRONT_CENTER, OMNI_C},
    {AV_CH_LOW_FREQUENCY, OMNI_LFE},
    {AV_CH_BACK_LEFT, OMNI_LB},
    {AV_CH_BACK_RIGHT, OMNI_RB},
    {AV_CH_FRONT_LEFT_OF_CENTER, OMNI_LSC},
    {AV_CH_FRONT_RIGHT_OF_CENTER, OMNI_RSC},
    {AV_CH_BACK_CENTER, OMNI_CB},
    {AV_CH_SIDE_LEFT, OMNI_LS},
    {AV_CH_SIDE_RIGHT, OMNI_RS},
    {AV_CH_TOP_CENTER, OMNI_TC},
    {AV_CH_TOP_FRONT_LEFT, OMNI_TFL},
    {AV_CH_TOP_FRONT_CENTER, OMNI_TFC},
    {AV_CH_TOP_FRONT_RIGHT, OMNI_TFR},
    {AV_CH_TOP_BACK_LEFT, OMNI_TBL},
    // AV_CH_TOP_BACK_CENTER has no counterpart and is deliberately absent.
    {AV_CH_TOP_BACK_RIGHT, OMNI_TBR},
    {AV_CH_WIDE_LEFT, OMNI_LW},
    {AV_CH_WIDE_RIGHT, OMNI_RW},
    {AV_CH_SURROUND_DIRECT_LEFT, OMNI_LSD},
    {AV_CH_SURROUND_DIRECT_RIGHT, OMNI_RSD},
    {AV_CH_LOW_FREQUENCY_2, OMNI_LFE2},
    {AV_CH_TOP_SIDE_LEFT, OMNI_TSL},
    {AV_CH_TOP_SIDE_RIGHT, OMNI_TSR},
};

//! \brief The renderer position for one mask bit, or -1 if it has none.
int LabelForBit(uint64_t bit)
{
  for (const auto& m : MAPPINGS)
  {
    if (m.bit == bit)
      return m.label;
  }
  return -1;
}

void PushU16(std::vector<uint8_t>& out, uint16_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PushU32(std::vector<uint8_t>& out, uint32_t v)
{
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
} // namespace

bool OmniphonyPcmChannelLabels(uint64_t mask, int channels, std::vector<uint8_t>& labels)
{
  if (channels <= 0 || channels > OMNI_PCM_MAX_CHANNELS)
    return false;

  // A mask whose population count differs from the decoded channel count is not
  // describing this stream. That is exactly the case CDVDAudioCodecFFmpeg's own
  // BuildChannelMap papers over by falling back to a default layout, and a
  // default layout is a guess at where the sound is.
  std::vector<uint8_t> mapped;
  mapped.reserve(static_cast<size_t>(channels));

  // Ascending bit order is interleave order for a native layout, so this walk
  // produces the labels in the order the samples arrive.
  for (int bit = 0; bit < 64; ++bit)
  {
    const uint64_t flag = static_cast<uint64_t>(1) << bit;
    if (!(mask & flag))
      continue;

    const int label = LabelForBit(flag);
    if (label < 0)
      return false;

    if (mapped.size() == static_cast<size_t>(channels))
      return false; // more set bits than channels; the mask is not this stream's

    mapped.push_back(static_cast<uint8_t>(label));
  }

  if (mapped.size() != static_cast<size_t>(channels))
    return false;

  labels.swap(mapped);
  return true;
}

std::vector<uint8_t> OmniphonyPcmHeader(const std::vector<uint8_t>& labels,
                                        uint32_t sampleRate,
                                        OmniphonyPcmEncoding encoding)
{
  if (labels.empty() || labels.size() > static_cast<size_t>(OMNI_PCM_MAX_CHANNELS) ||
      sampleRate == 0)
    return {};

  std::vector<uint8_t> header;
  header.reserve(OMNI_PCM_FIXED_HEADER_LEN + labels.size());

  header.push_back('O');
  header.push_back('P');
  header.push_back('C');
  header.push_back('M');
  PushU16(header, OMNI_PCM_VERSION);
  PushU16(header, static_cast<uint16_t>(labels.size()));
  PushU32(header, sampleRate);
  header.push_back(static_cast<uint8_t>(encoding));
  header.push_back(0); // reserved
  header.insert(header.end(), labels.begin(), labels.end());

  return header;
}
