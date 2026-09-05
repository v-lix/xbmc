/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDAudioCodecOmniphony.h"

/*
 * Apart from the codec's own translation unit so a test can link it without a
 * helper process, a settings component and an ActiveAE behind it. The same
 * reason OmniphonyPcmStream.cpp is its own file: the decision is pure, its
 * consequences are audible, and neither of those is true of the class it
 * serves.
 */

OmniphonyRateVerdict OmniphonyRateCheck(unsigned int reported,
                                        unsigned int opened,
                                        bool pcmPath,
                                        bool formatPublished)
{
  // Not a rate. Either the engine has not decoded a frame yet, or it predates
  // orender_decoded_sample_rate and the helper filled the field with a zero
  // rather than dropping it. An older helper omits the key entirely, and the
  // caller never asks.
  if (reported == 0 || reported == opened)
    return OmniphonyRateVerdict::Agrees;

  // The PCM path decodes in this class and resamples to `opened` before the
  // bytes ever reach the bridge, so the engine is being handed exactly what was
  // produced here. A disagreement is a bug here, not a demuxer that guessed.
  if (pcmPath)
    return OmniphonyRateVerdict::NotOnPcmPath;

  /*
   * Too late to fix, which is not the same as not worth knowing.
   *
   * Re-opening changes the format, and ActiveAE was configured from the first
   * block this codec handed over. A stream that changes rate part-way through a
   * film therefore cannot be followed - but it can be said out loud, which is
   * the whole difference between this and the silence the bug used to have.
   *
   * Checked before the range below because it is the more specific fact: a rate
   * this path could not have opened at is not worth reporting as unrenderable
   * once nothing can be done about it either way.
   */
  if (formatPublished)
    return OmniphonyRateVerdict::TooLate;

  /*
   * A rate this path cannot be re-opened at. The object path hands the bridge
   * undecoded bitstream and has no opportunity to resample, so the ceiling
   * ChooseRate applies at open applies here too - see there. Falling back costs
   * the listener the binaural render and gives them a film at the right speed,
   * which is the same trade RateAgrees makes and the same way round.
   */
  if (reported < OMNI_MIN_RATE || reported > OMNI_MAX_RATE)
    return OmniphonyRateVerdict::Unrenderable;

  return OmniphonyRateVerdict::Retune;
}
