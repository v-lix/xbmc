/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDCodecs/Audio/DVDAudioCodecOmniphony.h"

#include <gtest/gtest.h>

/*
 * The rate an engine reports is the only truthful answer to "what is this
 * stream actually decoding at", and acting on it wrongly is inaudible in the
 * two directions that matter: re-opening when nothing changed drops the render
 * and re-primes for no reason, and not re-opening when it did leaves a whole
 * film playing at the wrong speed. Neither shows up as an error anywhere, so
 * the decision is pinned here rather than left to a listen.
 */

namespace
{
constexpr unsigned int OPENED = 48000;

OmniphonyRateVerdict Check(unsigned int reported,
                           unsigned int opened = OPENED,
                           bool pcmPath = false,
                           bool formatPublished = false)
{
  return OmniphonyRateCheck(reported, opened, pcmPath, formatPublished);
}
} // namespace

TEST(TestOmniphonyRateCheck, AnAgreeingRateIsLeftAlone)
{
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(OPENED));
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(96000, 96000));
}

TEST(TestOmniphonyRateCheck, ZeroIsNotARate)
{
  // The engine has not decoded a frame yet, or is too old to have the symbol.
  // Either way it has said nothing, which is not the same as saying 48000 -
  // and must never be read as "disagrees with the rate we opened at".
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(0));
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(0, 96000));
}

TEST(TestOmniphonyRateCheck, TheXllCaseRetunes)
{
  // The whole reason this exists: DTS-HD MA whose 96 kHz XLL extension rides a
  // 48 kHz core. Kodi's parser reads the core's sync word and opens at 48; the
  // bridge decodes the extension at 96.
  EXPECT_EQ(OmniphonyRateVerdict::Retune, Check(96000));
}

TEST(TestOmniphonyRateCheck, ARateBelowTheOpenedOneRetunesToo)
{
  // The disagreement is not always upwards, and nothing here assumes it is.
  EXPECT_EQ(OmniphonyRateVerdict::Retune, Check(44100));
  EXPECT_EQ(OmniphonyRateVerdict::Retune, Check(48000, 96000));
}

TEST(TestOmniphonyRateCheck, ARateOutsideWhatThisPathRendersFallsBack)
{
  // Above the ceiling the object path cannot re-open: it hands the bridge
  // undecoded bitstream and has nowhere to resample. Ordinary decoding gives
  // the listener the film at the right speed, which beats a binaural render at
  // the wrong one.
  EXPECT_EQ(OmniphonyRateVerdict::Unrenderable, Check(OMNI_MAX_RATE + 1));
  EXPECT_EQ(OmniphonyRateVerdict::Unrenderable, Check(192000));
  EXPECT_EQ(OmniphonyRateVerdict::Unrenderable, Check(OMNI_MIN_RATE - 1));

  // The boundaries themselves are renderable.
  EXPECT_EQ(OmniphonyRateVerdict::Retune, Check(OMNI_MAX_RATE));
  EXPECT_EQ(OmniphonyRateVerdict::Retune, Check(OMNI_MIN_RATE));
}

TEST(TestOmniphonyRateCheck, ThePcmPathCannotDisagreeWithItself)
{
  // That path decodes and resamples in the codec, so the engine is handed
  // exactly what the codec produced. A disagreement means a bug here, and
  // restarting the helper would not fix it.
  EXPECT_EQ(OmniphonyRateVerdict::NotOnPcmPath, Check(96000, OPENED, true, false));

  // Still nothing to do even when the rate is one that could be opened at, and
  // still not a fall back: the audio is fine, the report is what is wrong.
  EXPECT_EQ(OmniphonyRateVerdict::NotOnPcmPath, Check(192000, OPENED, true, false));
}

TEST(TestOmniphonyRateCheck, OnceTheFormatIsPublishedNothingCanBeReopened)
{
  // ActiveAE was configured from the first block handed over, and re-opening
  // would leave it describing the old rate. All that is left is to say so.
  EXPECT_EQ(OmniphonyRateVerdict::TooLate, Check(96000, OPENED, false, true));

  // Including for a rate that would otherwise have fallen back - once nothing
  // can be done, "outside the range" is not the useful thing to report.
  EXPECT_EQ(OmniphonyRateVerdict::TooLate, Check(192000, OPENED, false, true));
}

TEST(TestOmniphonyRateCheck, AgreementOutranksEveryOtherReason)
{
  // A rate that agrees is the end of it, whatever else is true. Otherwise a
  // steady stream on the PCM path would log a bug report every frame.
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(OPENED, OPENED, true, true));
  EXPECT_EQ(OmniphonyRateVerdict::Agrees, Check(0, OPENED, true, true));
}
