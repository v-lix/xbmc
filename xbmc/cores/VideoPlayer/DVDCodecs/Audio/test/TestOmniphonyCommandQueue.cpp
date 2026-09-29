/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDCodecs/Audio/OmniphonyCommandQueue.h"

#include <vector>

#include <gtest/gtest.h>

/*
 * Every command below is a four-byte header of its tag, then a payload of that
 * many bytes filled with the tag too, so what is left in the queue can be read
 * back as the sequence of tags it holds.
 */

namespace
{
using Kind = COmniphonyCommandQueue::Kind;

constexpr size_t HEADER = 4;

void Push(COmniphonyCommandQueue& queue, Kind kind, uint8_t tag, size_t payload, double us = 0.0)
{
  const std::vector<uint8_t> header(HEADER, tag);
  const std::vector<uint8_t> body(payload, tag);
  queue.Push(kind, header.data(), header.size(), body.data(), body.size(), us);
}

std::vector<uint8_t> Unsent(const COmniphonyCommandQueue& queue)
{
  return std::vector<uint8_t>(queue.Data(), queue.Data() + queue.Bytes());
}

void PushAhead(COmniphonyCommandQueue& queue, uint8_t tag, size_t payload)
{
  const std::vector<uint8_t> header(HEADER, tag);
  const std::vector<uint8_t> body(payload, tag);
  queue.PushAhead(header.data(), header.size(), body.data(), body.size());
}

std::vector<uint8_t> Next(const COmniphonyCommandQueue& queue)
{
  return std::vector<uint8_t>(queue.Next(), queue.Next() + queue.NextBytes());
}

//! Everything the pump would write from here, a run at a time as it does.
std::vector<uint8_t> WriteAll(COmniphonyCommandQueue& queue)
{
  std::vector<uint8_t> written;
  while (queue.NextBytes())
  {
    const auto run = Next(queue);
    written.insert(written.end(), run.begin(), run.end());
    queue.Written(run.size());
  }
  return written;
}

std::vector<uint8_t> Command(uint8_t tag, size_t payload)
{
  return std::vector<uint8_t>(HEADER + payload, tag);
}

std::vector<uint8_t> Join(std::initializer_list<std::vector<uint8_t>> parts)
{
  std::vector<uint8_t> joined;
  for (const auto& part : parts)
    joined.insert(joined.end(), part.begin(), part.end());
  return joined;
}
} // namespace

TEST(TestOmniphonyCommandQueue, CountsBytesAndTimeUntilWritten)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 12, 32000.0);
  Push(queue, Kind::Audio, 2, 12, 32000.0);

  EXPECT_EQ(queue.Bytes(), 32u);
  EXPECT_DOUBLE_EQ(queue.Us(), 64000.0);

  // Part of a command still counts in full: the helper has not got all of it.
  queue.Written(10);
  EXPECT_EQ(queue.Bytes(), 22u);
  EXPECT_DOUBLE_EQ(queue.Us(), 64000.0);

  queue.Written(6);
  EXPECT_EQ(queue.Bytes(), 16u);
  EXPECT_DOUBLE_EQ(queue.Us(), 32000.0);

  queue.Written(16);
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, UntimedAudioCountsInBytesOnly)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 100, 0.0);
  Push(queue, Kind::Audio, 2, 100, -5.0);

  EXPECT_EQ(queue.Bytes(), 208u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, DropStaleTakesBackAudioNotYetStarted)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Audio, 2, 8, 10000.0);
  Push(queue, Kind::Audio, 3, 8, 10000.0);

  EXPECT_EQ(queue.DropStale(), 0u);
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, DropStaleFinishesTheCommandBeingWritten)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Audio, 2, 8, 10000.0);
  Push(queue, Kind::Audio, 3, 8, 10000.0);

  // Five bytes into the first command: the rest of it has to follow, or the
  // helper would read the next header out of the middle of its payload.
  queue.Written(5);
  EXPECT_EQ(queue.DropStale(), 0u);

  const auto whole = Command(1, 8);
  EXPECT_EQ(Unsent(queue), std::vector<uint8_t>(whole.begin() + 5, whole.end()));
  EXPECT_DOUBLE_EQ(queue.Us(), 10000.0);

  queue.Written(queue.Bytes());
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, DropStaleTakesBackACommandWrittenUpToItsStart)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Audio, 2, 8, 10000.0);

  // Exactly the first command has gone: none of the second has.
  queue.Written(HEADER + 8);
  EXPECT_EQ(queue.DropStale(), 0u);
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, DropStaleCountsTheResetsItSupersedes)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Reset, 2, 0);
  Push(queue, Kind::Audio, 3, 8, 10000.0);
  Push(queue, Kind::Reset, 4, 0);
  Push(queue, Kind::Audio, 5, 8, 10000.0);

  EXPECT_EQ(queue.DropStale(), 2u);
  EXPECT_EQ(queue.Bytes(), 0u);
}

TEST(TestOmniphonyCommandQueue, DropStaleLeavesAResetAlreadyStarted)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Reset, 2, 0);
  Push(queue, Kind::Audio, 3, 8, 10000.0);

  // Into the reset's header: it will be answered, so it stays counted.
  queue.Written(HEADER + 8 + 1);
  EXPECT_EQ(queue.DropStale(), 0u);
  EXPECT_EQ(Unsent(queue), std::vector<uint8_t>(HEADER - 1, 2));
}

TEST(TestOmniphonyCommandQueue, DropStaleKeepsControlCommandsInOrder)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Control, 2, 3);
  Push(queue, Kind::Audio, 3, 8, 10000.0);
  Push(queue, Kind::Reset, 4, 0);
  Push(queue, Kind::Control, 5, 6);
  Push(queue, Kind::Audio, 6, 8, 10000.0);

  queue.Written(2);
  EXPECT_EQ(queue.DropStale(), 1u);

  const auto first = Command(1, 8);
  EXPECT_EQ(Unsent(queue), Join({std::vector<uint8_t>(first.begin() + 2, first.end()),
                                 Command(2, 3), Command(5, 6)}));
  EXPECT_DOUBLE_EQ(queue.Us(), 10000.0);

  // What was kept is still framed right: writing it off one byte at a time
  // retires each command exactly at its end.
  const size_t rest = queue.Bytes();
  for (size_t i = 0; i < rest; ++i)
    queue.Written(1);
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, CommandsQueuedAfterADropFollowOn)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Audio, 2, 8, 10000.0);
  queue.Written(3);
  queue.DropStale();

  Push(queue, Kind::Reset, 3, 0);
  Push(queue, Kind::Audio, 4, 8, 20000.0);

  const auto first = Command(1, 8);
  EXPECT_EQ(Unsent(queue), Join({std::vector<uint8_t>(first.begin() + 3, first.end()),
                                 Command(3, 0), Command(4, 8)}));
  EXPECT_DOUBLE_EQ(queue.Us(), 30000.0);

  // And a second seek before any of that has gone takes back the first one's
  // reset and audio, leaving the partial command alone.
  EXPECT_EQ(queue.DropStale(), 1u);
  EXPECT_EQ(Unsent(queue), std::vector<uint8_t>(first.begin() + 3, first.end()));
}

TEST(TestOmniphonyCommandQueue, SurvivesReclaimingWhatHasBeenWritten)
{
  // Enough that the written part is reclaimed while commands are still
  // waiting, which moves the bytes the records point into.
  COmniphonyCommandQueue queue;
  constexpr size_t PAYLOAD = 4092;
  constexpr int COUNT = 200;
  for (int i = 0; i < COUNT; ++i)
    Push(queue, Kind::Audio, static_cast<uint8_t>(i), PAYLOAD, 1000.0);

  const size_t each = HEADER + PAYLOAD;
  queue.Written(each * 150 + 7);
  EXPECT_EQ(queue.Bytes(), each * 50 - 7);
  EXPECT_DOUBLE_EQ(queue.Us(), 50000.0);

  EXPECT_EQ(queue.DropStale(), 0u);
  const auto partial = Command(150, PAYLOAD);
  EXPECT_EQ(Unsent(queue), std::vector<uint8_t>(partial.begin() + 7, partial.end()));
  EXPECT_DOUBLE_EQ(queue.Us(), 1000.0);

  Push(queue, Kind::Reset, 0xEE, 0);
  queue.Written(queue.Bytes() - 1);
  EXPECT_EQ(Unsent(queue), std::vector<uint8_t>(1, 0xEE));
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, ClearForgetsEverything)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Reset, 2, 0);
  queue.Written(3);
  queue.Clear();

  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
  EXPECT_EQ(queue.DropStale(), 0u);

  Push(queue, Kind::Audio, 3, 8, 10000.0);
  EXPECT_EQ(Unsent(queue), Command(3, 8));
  EXPECT_DOUBLE_EQ(queue.Us(), 10000.0);
}

TEST(TestOmniphonyCommandQueue, AheadWaitsOnlyForTheCommandBeingWritten)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Audio, 2, 8, 10000.0);
  queue.Written(5);
  PushAhead(queue, 9, 2);

  // Not counted as audio or as backlog, and not in the way of a seek.
  EXPECT_EQ(queue.Bytes(), 2 * (HEADER + 8) - 5);
  EXPECT_DOUBLE_EQ(queue.Us(), 20000.0);

  const auto first = Command(1, 8);
  EXPECT_EQ(WriteAll(queue), Join({std::vector<uint8_t>(first.begin() + 5, first.end()),
                                   Command(9, 2), Command(2, 8)}));
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}

TEST(TestOmniphonyCommandQueue, AheadGoesFirstFromABoundary)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  PushAhead(queue, 9, 2);
  EXPECT_EQ(WriteAll(queue), Join({Command(9, 2), Command(1, 8)}));

  // And with nothing else queued at all.
  PushAhead(queue, 8, 2);
  EXPECT_EQ(WriteAll(queue), Command(8, 2));
}

TEST(TestOmniphonyCommandQueue, OnlyTheLatestAheadGoes)
{
  COmniphonyCommandQueue queue;
  PushAhead(queue, 9, 2);
  PushAhead(queue, 8, 2);
  EXPECT_EQ(WriteAll(queue), Command(8, 2));
}

TEST(TestOmniphonyCommandQueue, AnAheadAlreadyStartedFinishesBeforeANewerOne)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  PushAhead(queue, 9, 4);
  queue.Written(3);
  PushAhead(queue, 8, 4);
  PushAhead(queue, 7, 4);

  const auto started = Command(9, 4);
  EXPECT_EQ(WriteAll(queue), Join({std::vector<uint8_t>(started.begin() + 3, started.end()),
                                   Command(7, 4), Command(1, 8)}));
}

TEST(TestOmniphonyCommandQueue, AheadSurvivesADropAndGoesWithAClear)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Reset, 2, 0);
  PushAhead(queue, 9, 2);
  EXPECT_EQ(queue.DropStale(), 1u);
  EXPECT_EQ(WriteAll(queue), Command(9, 2));

  PushAhead(queue, 8, 2);
  queue.Written(1);
  queue.Clear();
  EXPECT_EQ(queue.NextBytes(), 0u);
  Push(queue, Kind::Audio, 3, 8, 10000.0);
  EXPECT_EQ(WriteAll(queue), Command(3, 8));
}

TEST(TestOmniphonyCommandQueue, AheadKeepsTheFramingAByteAtATime)
{
  COmniphonyCommandQueue queue;
  Push(queue, Kind::Audio, 1, 8, 10000.0);
  Push(queue, Kind::Control, 2, 3);
  Push(queue, Kind::Audio, 3, 8, 10000.0);

  // A pipe that takes one byte per write. Put ahead part-way through the first
  // command, it waits for that command's end; two more while it is going out,
  // and only the latest follows it; one part-way through the last command
  // waits for that one's end.
  std::vector<uint8_t> written;
  for (size_t i = 0; queue.NextBytes(); ++i)
  {
    if (i == 2)
      PushAhead(queue, 0x40, 1);
    if (i == 13)
      PushAhead(queue, 0x41, 1);
    if (i == 14)
      PushAhead(queue, 0x42, 1);
    if (i == 30)
      PushAhead(queue, 0x43, 1);
    written.push_back(*queue.Next());
    queue.Written(1);
  }

  EXPECT_EQ(written, Join({Command(1, 8), Command(0x40, 1), Command(0x42, 1), Command(2, 3),
                           Command(3, 8), Command(0x43, 1)}));
  EXPECT_EQ(queue.Bytes(), 0u);
  EXPECT_DOUBLE_EQ(queue.Us(), 0.0);
}
