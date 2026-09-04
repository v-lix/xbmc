/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDCodecs/Audio/OmniphonyPcmStream.h"

extern "C"
{
#include <libavutil/channel_layout.h>
}

#include <gtest/gtest.h>

/*
 * These bytes are the renderer's ABI. The parser on the other side lives in
 * omniphony-renderer/pcm_bridge/src/header.rs, in a different language and a
 * different process, so nothing but a test holds the two spellings together.
 * The label values below are RChannelLabel's discriminants in bridge_api.
 */

namespace
{
constexpr uint8_t L = 0;
constexpr uint8_t R = 1;
constexpr uint8_t C = 2;
constexpr uint8_t LFE = 3;
constexpr uint8_t LS = 4; // side left
constexpr uint8_t RS = 5; // side right
constexpr uint8_t TFL = 6;
constexpr uint8_t TFR = 7;
constexpr uint8_t TBL = 10;
constexpr uint8_t TBR = 11;
constexpr uint8_t LB = 14; // back left
constexpr uint8_t RB = 15; // back right

std::vector<uint8_t> LabelsOf(uint64_t mask, int channels)
{
  std::vector<uint8_t> labels;
  EXPECT_TRUE(OmniphonyPcmChannelLabels(mask, channels, labels));
  return labels;
}
} // namespace

TEST(TestOmniphonyPcmStream, LabelsStereo)
{
  EXPECT_EQ(LabelsOf(AV_CH_LAYOUT_STEREO, 2), (std::vector<uint8_t>{L, R}));
}

TEST(TestOmniphonyPcmStream, LabelsMonoIsCentre)
{
  EXPECT_EQ(LabelsOf(AV_CH_LAYOUT_MONO, 1), (std::vector<uint8_t>{C}));
}

//! The distinction a bare channel count cannot express, and the reason this
//! wire format carries labels at all.
TEST(TestOmniphonyPcmStream, SideAndBackFivePointOneDiffer)
{
  const auto side = LabelsOf(AV_CH_LAYOUT_5POINT1, 6);
  const auto back = LabelsOf(AV_CH_LAYOUT_5POINT1_BACK, 6);

  EXPECT_EQ(side, (std::vector<uint8_t>{L, R, C, LFE, LS, RS}));
  EXPECT_EQ(back, (std::vector<uint8_t>{L, R, C, LFE, LB, RB}));
  EXPECT_NE(side, back);
}

//! Ascending mask-bit order is interleave order, which puts the back pair
//! (bits 4 and 5) before the side pair (bits 9 and 10).
TEST(TestOmniphonyPcmStream, SevenPointOneIsBackThenSide)
{
  EXPECT_EQ(LabelsOf(AV_CH_LAYOUT_7POINT1, 8),
            (std::vector<uint8_t>{L, R, C, LFE, LB, RB, LS, RS}));
}

TEST(TestOmniphonyPcmStream, SevenPointOnePointFourCarriesHeight)
{
  EXPECT_EQ(LabelsOf(AV_CH_LAYOUT_7POINT1POINT4_BACK, 12),
            (std::vector<uint8_t>{L, R, C, LFE, LB, RB, LS, RS, TFL, TFR, TBL, TBR}));
}

TEST(TestOmniphonyPcmStream, RejectsACountThatDisagreesWithTheMask)
{
  std::vector<uint8_t> labels;
  // Six set bits, eight channels claimed: the mask is not this stream's.
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_5POINT1, 8, labels));
  EXPECT_TRUE(labels.empty());
}

TEST(TestOmniphonyPcmStream, RejectsAPositionTheRendererCannotPlace)
{
  std::vector<uint8_t> labels;
  // TOP_BACK_CENTER has no counterpart, so the whole layout is refused rather
  // than the channel being placed approximately.
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_STEREO | AV_CH_TOP_BACK_CENTER, 3, labels));
  // 22.2 carries that channel and the BOTTOM_* family too.
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_22POINT2, 24, labels));
}

TEST(TestOmniphonyPcmStream, RejectsCountsOutOfRange)
{
  std::vector<uint8_t> labels;
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_STEREO, 0, labels));
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_STEREO, -1, labels));
  EXPECT_FALSE(OmniphonyPcmChannelLabels(AV_CH_LAYOUT_STEREO, OMNI_PCM_MAX_CHANNELS + 1, labels));
}

//! Byte for byte, because the reader is a parser in another process.
TEST(TestOmniphonyPcmStream, HeaderBytes)
{
  const auto header =
      OmniphonyPcmHeader(LabelsOf(AV_CH_LAYOUT_STEREO, 2), 48000, OmniphonyPcmEncoding::Float32);

  const std::vector<uint8_t> expected{
      'O',  'P',  'C',  'M', // magic
      0x01, 0x00, // version 1, little endian
      0x02, 0x00, // two channels
      0x80, 0xBB, 0x00, 0x00, // 48000 Hz
      0x01, // Float32
      0x00, // reserved
      L,    R, // labels
  };
  EXPECT_EQ(header, expected);
  EXPECT_EQ(header.size(), OMNI_PCM_FIXED_HEADER_LEN + 2);
}

TEST(TestOmniphonyPcmStream, HeaderCarriesTheEncodingItWasAskedFor)
{
  const auto labels = LabelsOf(AV_CH_LAYOUT_STEREO, 2);
  EXPECT_EQ(OmniphonyPcmHeader(labels, 48000, OmniphonyPcmEncoding::I32Scaled24)[12], 0);
  EXPECT_EQ(OmniphonyPcmHeader(labels, 48000, OmniphonyPcmEncoding::Float32)[12], 1);
}

TEST(TestOmniphonyPcmStream, HeaderRejectsWhatTheBridgeWould)
{
  const auto labels = LabelsOf(AV_CH_LAYOUT_STEREO, 2);
  EXPECT_TRUE(OmniphonyPcmHeader({}, 48000, OmniphonyPcmEncoding::Float32).empty());
  EXPECT_TRUE(OmniphonyPcmHeader(labels, 0, OmniphonyPcmEncoding::Float32).empty());
  EXPECT_TRUE(OmniphonyPcmHeader(std::vector<uint8_t>(OMNI_PCM_MAX_CHANNELS + 1, L), 48000,
                                 OmniphonyPcmEncoding::Float32)
                  .empty());
}

//! A 44.1 kHz source is resampled to 48 kHz before it reaches the bridge, but
//! the header still has to be able to say any rate: nothing here assumes one.
TEST(TestOmniphonyPcmStream, HeaderEncodesTheRateLittleEndian)
{
  const auto header =
      OmniphonyPcmHeader(LabelsOf(AV_CH_LAYOUT_MONO, 1), 44100, OmniphonyPcmEncoding::Float32);
  EXPECT_EQ(header[8], 0x44);
  EXPECT_EQ(header[9], 0xAC);
  EXPECT_EQ(header[10], 0x00);
  EXPECT_EQ(header[11], 0x00);
}
