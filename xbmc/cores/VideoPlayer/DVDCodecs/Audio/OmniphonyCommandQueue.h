/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/*!
 * \brief Commands framed for the helper that have not been written to it yet.
 *
 * The bytes are kept in one piece, so the pump thread writes as much as the pipe
 * will take in a single call - a TrueHD stream is twelve hundred commands a
 * second, and one write each would cost the pump more than the audio does.
 * Alongside them is a record of where each command starts and ends, which is
 * what a seek needs: the helper works through its input strictly in order, so
 * a reset queued behind seconds of audio is answered only once all of it has
 * been decoded and rendered, and every block of that is thrown away on arrival.
 * With the boundaries known, the audio nobody will hear can be taken back
 * before it is sent instead.
 *
 * A command the pump has started writing is never taken back: the helper has
 * part of it, and a command cut short would frame everything after it wrong.
 */
class COmniphonyCommandQueue
{
public:
  enum class Kind
  {
    Audio, //!< input to render: stale once a reset is queued behind it
    Reset, //!< a reset: pointless once another is queued behind it
    Control //!< anything else, which is kept whatever follows it
  };

  /*!
   * \brief Queue one command, its header and payload, as it is to be written.
   *
   * \p us is how much audio it carries, in microseconds - 0 where there is none
   * or it cannot be told, which leaves it out of Us() but not out of Bytes().
   */
  void Push(Kind kind,
            const uint8_t* header,
            size_t headerLen,
            const uint8_t* payload,
            size_t payloadLen,
            double us);

  /*!
   * \brief Queue a command to go out at the next boundary between commands,
   * ahead of everything waiting, in place of one queued this way and not yet
   * started.
   *
   * For a command whose latest copy is the only one that matters and which
   * cannot wait its turn: where the listener is, which behind a second of
   * queued audio would reach the helper a second late. Kept apart from the
   * commands above - none of Bytes(), Us() or DropStale() sees it - and
   * never taken back.
   */
  void PushAhead(const uint8_t* header,
                 size_t headerLen,
                 const uint8_t* payload,
                 size_t payloadLen);

  //! \brief The bytes not yet written, oldest first, Bytes() of them.
  const uint8_t* Data() const { return m_bytes.data() + m_sent; }
  size_t Bytes() const { return m_bytes.size() - m_sent; }

  //! \brief What to write next, NextBytes() of it: the command put ahead, once
  //! the one being written has gone out whole, and Data() otherwise - only as
  //! far as the end of the command being written while one is waiting to go
  //! ahead, which is what gives it a boundary to go at.
  const uint8_t* Next() const;
  size_t NextBytes() const;

  //! \brief Audio in the commands not yet written in full, in microseconds.
  double Us() const { return m_us; }

  //! \brief \p n of the bytes Next() offered have been written.
  void Written(size_t n);

  /*!
   * \brief Take back every command not yet started that a reset about to be
   * queued makes pointless: all of the audio, and the resets it supersedes.
   *
   * Control commands stay, in order. Returns how many resets were taken back,
   * which the caller has counted as sent and must now count as answered.
   */
  unsigned int DropStale();

  void Clear();

private:
  struct Command
  {
    uint64_t start; //!< offset in the stream of everything ever queued
    uint64_t end;
    Kind kind;
    double us;
  };

  //! \brief Whether Next() is the command put ahead: it has been started, or
  //! nothing else is part-way through being written.
  bool AheadIsNext() const;

  std::vector<uint8_t> m_bytes;
  size_t m_sent{0}; //!< how much of m_bytes has been written
  std::vector<uint8_t> m_ahead; //!< the command put ahead, whole
  size_t m_aheadSent{0}; //!< how much of m_ahead has been written
  std::vector<uint8_t> m_aheadNext; //!< a newer one, put ahead while m_ahead was going out
  uint64_t m_base{0}; //!< the stream offset of m_bytes[0]
  std::deque<Command> m_commands; //!< not yet written in full, oldest first
  double m_us{0.0};
};
