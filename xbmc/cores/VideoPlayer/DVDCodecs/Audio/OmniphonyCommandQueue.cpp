/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyCommandQueue.h"

#include <algorithm>

/*
 * Apart from the codec for the reason OmniphonyRateCheck.cpp is: a test can
 * link it without a helper process behind it.
 */

void COmniphonyCommandQueue::Push(Kind kind,
                                  const uint8_t* header,
                                  size_t headerLen,
                                  const uint8_t* payload,
                                  size_t payloadLen,
                                  double us)
{
  const uint64_t start = m_base + m_bytes.size();
  m_bytes.insert(m_bytes.end(), header, header + headerLen);
  if (payloadLen)
    m_bytes.insert(m_bytes.end(), payload, payload + payloadLen);
  m_commands.push_back({start, start + headerLen + payloadLen, kind, us > 0.0 ? us : 0.0});
  m_us += m_commands.back().us;
}

void COmniphonyCommandQueue::PushAhead(const uint8_t* header,
                                       size_t headerLen,
                                       const uint8_t* payload,
                                       size_t payloadLen)
{
  // One already started has to finish - the helper has part of it - so a newer
  // one waits behind it rather than replacing it.
  std::vector<uint8_t>& slot = m_aheadSent ? m_aheadNext : m_ahead;
  slot.assign(header, header + headerLen);
  if (payloadLen)
    slot.insert(slot.end(), payload, payload + payloadLen);
}

bool COmniphonyCommandQueue::AheadIsNext() const
{
  if (m_ahead.empty())
    return false;
  if (m_aheadSent)
    return true;
  // The oldest command not written in full, if it has not been started either,
  // leaves the stream at a boundary.
  return m_commands.empty() || m_commands.front().start == m_base + m_sent;
}

const uint8_t* COmniphonyCommandQueue::Next() const
{
  return AheadIsNext() ? m_ahead.data() + m_aheadSent : Data();
}

size_t COmniphonyCommandQueue::NextBytes() const
{
  if (AheadIsNext())
    return m_ahead.size() - m_aheadSent;
  // Otherwise a write would run on through every command waiting, and one to
  // go ahead of them would only get its turn when a write happened to end on
  // a boundary - which, with the pipe full, is almost never.
  if (!m_ahead.empty() && !m_commands.empty())
    return static_cast<size_t>(m_commands.front().end - (m_base + m_sent));
  return Bytes();
}

void COmniphonyCommandQueue::Written(size_t n)
{
  if (AheadIsNext())
  {
    m_aheadSent += std::min(n, m_ahead.size() - m_aheadSent);
    if (m_aheadSent == m_ahead.size())
    {
      m_ahead.swap(m_aheadNext);
      m_aheadNext.clear();
      m_aheadSent = 0;
    }
    return;
  }

  m_sent += std::min(n, Bytes());

  const uint64_t at = m_base + m_sent;
  while (!m_commands.empty() && m_commands.front().end <= at)
  {
    m_us -= m_commands.front().us;
    m_commands.pop_front();
  }
  // A sum kept by adding and subtracting drifts; an empty queue carries none.
  if (m_commands.empty() || m_us < 0.0)
    m_us = 0.0;

  if (m_sent == m_bytes.size())
  {
    m_base += m_sent;
    m_bytes.clear();
    m_sent = 0;
  }
  // A helper that stays behind never lets the queue empty, and what has
  // already gone out was then kept for as long as that lasted. Dropped once
  // there is at least as much of it as is waiting, so no byte is moved more
  // often than a byte is written.
  else if (m_sent >= (1u << 18) && m_sent >= m_bytes.size() - m_sent)
  {
    m_bytes.erase(m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_sent));
    m_base += m_sent;
    m_sent = 0;
  }
}

unsigned int COmniphonyCommandQueue::DropStale()
{
  // The first command the helper has not been given any of. Everything before
  // it has been written in part or in full and has to go out as it is.
  const uint64_t at = m_base + m_sent;
  const auto first = std::find_if(m_commands.begin(), m_commands.end(),
                                  [at](const Command& command) { return command.start >= at; });
  if (first == m_commands.end())
    return 0;

  std::vector<uint8_t> kept;
  std::deque<Command> keptCommands;
  const uint64_t from = first->start;
  unsigned int resets = 0;
  for (auto command = first; command != m_commands.end(); ++command)
  {
    if (command->kind == Kind::Audio)
    {
      m_us -= command->us;
      continue;
    }
    if (command->kind == Kind::Reset)
    {
      ++resets;
      continue;
    }

    const uint64_t start = from + kept.size();
    kept.insert(kept.end(), m_bytes.begin() + static_cast<std::ptrdiff_t>(command->start - m_base),
                m_bytes.begin() + static_cast<std::ptrdiff_t>(command->end - m_base));
    keptCommands.push_back({start, from + kept.size(), command->kind, command->us});
  }

  m_commands.erase(first, m_commands.end());
  m_commands.insert(m_commands.end(), keptCommands.begin(), keptCommands.end());
  m_bytes.resize(static_cast<size_t>(from - m_base));
  m_bytes.insert(m_bytes.end(), kept.begin(), kept.end());
  if (m_commands.empty() || m_us < 0.0)
    m_us = 0.0;
  return resets;
}

void COmniphonyCommandQueue::Clear()
{
  m_base += m_bytes.size();
  m_bytes.clear();
  m_sent = 0;
  m_commands.clear();
  m_us = 0.0;
  m_ahead.clear();
  m_aheadSent = 0;
  m_aheadNext.clear();
}
