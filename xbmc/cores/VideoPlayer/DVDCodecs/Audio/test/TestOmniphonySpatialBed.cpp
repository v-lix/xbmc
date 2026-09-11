/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDCodecs/Audio/DVDAudioCodecOmniphony.h"

#include <gtest/gtest.h>

// The bed strings are the ones the helper packs from the engine's labels:
// comma separated, no spaces (see describe_bed in omniphony-helper.c).

TEST(TestOmniphonySpatialBed, NamesABedCarryingObjectsByItsLayout)
{
  // DTS:X D4: 7.1 bed, the fixed height quartet, five objects on top. With
  // objects to report the bed is context, so it is written the compact way.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs,Lb,Rb,Tfl,Tfr,Tbl,Tbr", 5),
            "7.1.4 + 5 Objects");
}

TEST(TestOmniphonySpatialBed, NamesAHeightBedCarryingNoObjects)
{
  // A presentation that places a height quartet and nothing above it still has
  // something to say, which is the whole reason this is not gated on objects.
  // Here the heights are the news, so they are spelled out rather than folded
  // into a third number.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs,Tfl,Tfr,Tbl,Tbr", 0),
            "5.1 + 4 Heights");
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs,Tfl,Tfr,Tbl,Tbr", -1),
            "5.1 + 4 Heights");
}

TEST(TestOmniphonySpatialBed, NamesAFlatBedCarryingObjects)
{
  // No heights, so no third number - but a floor is still a layout, and saying
  // it beats listing the six labels it is made of.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs", 11), "5.1 + 11 Objects");
}

TEST(TestOmniphonySpatialBed, SingularsAreSpelledSingular)
{
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs,Tfc", 1), "5.1.1 + 1 Object");
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs,Tfc", 0), "5.1 + 1 Height");
}

TEST(TestOmniphonySpatialBed, CountsEveryOverheadPositionAsAHeight)
{
  // All eight names the engine has for an overhead position, and the second
  // LFE, which belongs to the floor's point-something rather than above it.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,LFE2,Ls,Rs,Tfl,Tfr,Tsl,Tsr,Tbl,Tbr,Tfc,Tc", 2),
            "5.2.8 + 2 Objects");
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,LFE2,Ls,Rs,Tfl,Tfr,Tsl,Tsr,Tbl,Tbr,Tfc,Tc", 0),
            "5.2 + 8 Heights");
}

TEST(TestOmniphonySpatialBed, AFloorlessBedIsLeftToTheLabelList)
{
  // "0.1" is not a layout anyone writes, and there is nothing overhead to
  // spell out instead, so this returns empty and the caller keeps
  // "LFE + 15 Objects" - the sentence that already reads correctly there.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("LFE", 15), "");
  EXPECT_EQ(OmniphonyDescribeSpatialBed("", 15), "");
}

TEST(TestOmniphonySpatialBed, SaysNothingAboutAPlainBedWithNoObjects)
{
  // Neither heights nor objects: Player.Process(audiochannels) already says
  // this, and the row exists to add to that rather than repeat it.
  EXPECT_EQ(OmniphonyDescribeSpatialBed("L,R,C,LFE,Ls,Rs", 0), "");
}

TEST(TestOmniphonySpatialBed, NamesHeightsWithNoFloorUnderThem)
{
  // Nothing produces this today; it is here so that if something ever does,
  // the row says what arrived rather than "0.0.2 + ...".
  EXPECT_EQ(OmniphonyDescribeSpatialBed("Tfl,Tfr", 3), "2 Heights + 3 Objects");
}

TEST(TestOmniphonySpatialBed, ToleratesSpacingAndEmptyFields)
{
  // The helper packs without spaces, but nothing downstream should depend on
  // that to avoid miscounting a channel.
  EXPECT_EQ(OmniphonyDescribeSpatialBed(" L , R , C , LFE , Ls , Rs , Tfl , Tfr ,", 0),
            "5.1 + 2 Heights");
  EXPECT_EQ(OmniphonyDescribeSpatialBed(" L , R , C , LFE , Ls , Rs , Tfl , Tfr ,", 4),
            "5.1.2 + 4 Objects");
}

// The decoder's own name for a stream the container never named. The strings
// are the ones the helper packs from orender_presentation_name, with its
// underscores already turned back into spaces (see the presentation= parse).

TEST(TestOmniphonyPresentation, NamesAnAuroCarrierByItsLayout)
{
  // What the release is sold as and what a listener buys speakers for. Auro is
  // channel-based, so no object clause follows.
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 11.1", 0), "Auro 11.1");
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 9.1", 0), "Auro 9.1");
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 13.1", 0), "Auro 13.1");
}

TEST(TestOmniphonyPresentation, NamesOneThatHasNotReportedObjectsYet)
{
  // -1 is "the helper has not said", which for a carrier that reports no
  // objects for its whole length is the value the row lives with.
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 11.1", -1), "Auro 11.1");
}

TEST(TestOmniphonyPresentation, SaysNothingWhenTheContainerAlreadyNamedTheStream)
{
  // Empty is every stream but the ones that hide what they are: Atmos, DTS:X
  // and plain multichannel all arrive named, and the caller falls back to the
  // bed-derived form.
  EXPECT_EQ(OmniphonyDescribePresentation("", 0), "");
  EXPECT_EQ(OmniphonyDescribePresentation("", 15), "");
}

TEST(TestOmniphonyPresentation, KeepsAnObjectCountAlongsideAName)
{
  // Nothing produces this today; it is here so that if something ever does,
  // the row loses neither half.
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 11.1", 5), "Auro 11.1 + 5 Objects");
  EXPECT_EQ(OmniphonyDescribePresentation("Auro 11.1", 1), "Auro 11.1 + 1 Object");
}
