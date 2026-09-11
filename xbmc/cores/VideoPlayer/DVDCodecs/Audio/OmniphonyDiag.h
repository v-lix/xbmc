/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * TEMPORARY instrumentation. Delete this file, OmniphonyDiag.cpp, their two
 * CMakeLists entries and the call sites marked "OmniDiag" before this branch
 * is finished with. Nothing else should come to depend on it.
 *
 * It exists to answer questions the existing logs cannot, without the cost of
 * component logging: on a TrueHD stream the AV-timing and Audio components
 * fire over a thousand times a second, which loads the box enough to change
 * the thing being measured. Everything here goes to the ordinary debug log at
 * LOGDEBUG with no component mask, about eleven short lines a second, and
 * nothing is sampled at all unless debug logging is on.
 *
 *   Which thread is on the saturated CPU, and which core it is on.
 *   How long CAMLCodec::PollFrame actually blocks.
 *   Whether the renderer keeps up, how deep the reserve is, and - the question
 *   the bank change turns on - whether the player keeps feeding it while the
 *   sink is not taking audio.
 *
 * Two nested lifetimes. The playback one carries the CPU, thread and poll
 * sampling and is driven by the video decoder, so a passthrough film can be
 * measured as a control with no renderer in the picture at all. The renderer
 * one is nested inside it and carries everything about the helper.
 *
 * Every renderer-side producer carries the run token its codec was given.
 * A replacement codec can open before the codec it replaces is disposed, and
 * without the token the old helper's output would be credited to the new run.
 *
 * Reading the numbers, and what they are not:
 *
 *   The output rate is a DELIVERY rate - frames arriving divided by wall time.
 *   It is not the renderer's capacity. While the player only feeds at playback
 *   speed a renderer capable of twice real time still reads 1.00x, and a pause
 *   or a seek reads as a deficit.
 *
 *   "cap" on the thread line is CPU efficiency, not throughput: audio-seconds
 *   produced per CPU-second the helper actually spent, and only meaningful
 *   when it had work to do. It says what the render costs, not what the
 *   pipeline could carry - a helper that is fast per CPU-second can still be
 *   held to real time by scheduling, by the pipe, or by the feed.
 *
 *   "handed" counts frames given to CVideoPlayerAudio, not frames the sink
 *   accepted. A blocked sink shows up as feeding stopping, not as this falling.
 */

#include "threads/CriticalSection.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <sys/types.h>

class COmniphonyDiag
{
public:
  static COmniphonyDiag& Get();

  //! \brief Why AddData turned a packet away, for the feed line.
  enum class Refusal
  {
    BankFull, //!< the reserve is at OMNI_BANK_MS
    InputQueue, //!< the helper has stopped reading what it is sent
  };

  //! \brief Playback opened or closed. Idempotent; safe to pair unevenly.
  void StartPlayback();
  void StopPlayback();

  /*!
   * \brief A renderer run began. Returns its token, or zero if not sampling.
   *
   * Every producer below carries this back, and one carrying a stale token is
   * ignored - see the note above.
   */
  uint64_t AttachHelper(pid_t helperPid, unsigned int rate);
  void DetachHelper(uint64_t run);

  //! \brief Begin the renderer accounting again: a seek, a re-open, a fallback.
  void Mark(uint64_t run, const char* why);

  //! \brief One block came back from the helper. Pump thread.
  void OnRendered(uint64_t run, uint32_t frames);

  //! \brief Rendered PCM the pump is holding, after any change. Pump thread.
  void SetPumpHeld(uint64_t run, uint64_t frames);

  /*!
   * \brief Rendered PCM the codec is holding.
   *
   * Published wherever the bank moves rather than only on a successful
   * hand-off, so it stays true while the audio thread is parked in the sink.
   */
  void SetCodecHeld(uint64_t run, int frames);

  /*!
   * \brief Encoded input written to the helper but not yet taken off us.
   *
   * Separate from the bank and published from the pump as well as the codec:
   * the pump drains this queue on its own thread, so a codec-side snapshot
   * alone goes stale exactly when the audio thread is blocked - which is when
   * the figure matters.
   */
  void SetInputQueue(uint64_t run, uint64_t bytes);

  //! \brief One block went to the player - not to the sink. Audio thread.
  void OnHandedOut(uint64_t run, uint32_t frames);

  //! \brief One access unit, or one PCM chunk, written to the helper.
  void OnFed(uint64_t run, size_t bytes);

  //! \brief A packet the codec would not take, and why.
  void OnFeedRefused(uint64_t run, Refusal why);

  //! \brief Time AddData spent waiting for the helper's input queue to drain.
  void OnFeedWait(uint64_t run, uint64_t microseconds);

  //! \brief One CAMLCodec::PollFrame returned. GUI thread; playback-scoped.
  void OnPollFrame(int64_t microseconds);

private:
  COmniphonyDiag() = default;
  ~COmniphonyDiag();
  COmniphonyDiag(const COmniphonyDiag&) = delete;
  COmniphonyDiag& operator=(const COmniphonyDiag&) = delete;

  class CSampler;

  //! \brief Start or stop the sampler to match \ref m_playback and \ref m_run.
  void Sync();

  /*!
   * \brief The four lifetime calls, which is all that touches \ref m_sampler.
   *
   * Playback comes from the video decoder and the renderer run from the audio
   * thread, so the two can arrive at once. The producers below never take it -
   * they are on the hot path and read atomics only.
   */
  CCriticalSection m_lifetime;

  std::unique_ptr<CSampler> m_sampler;
  bool m_playback{false};
  uint64_t m_nextRun{0};

  //! \brief \ref m_sampler as the producers may read it.
  std::atomic<bool> m_sampling{false};

  //! \brief The renderer run producers must quote, or zero for none.
  std::atomic<uint64_t> m_run{0};
  std::atomic<int> m_helperPid{-1};
  std::atomic<uint32_t> m_rate{48000};

  std::atomic<uint64_t> m_renderedFrames{0};
  std::atomic<uint64_t> m_handedFrames{0};
  std::atomic<uint64_t> m_pumpHeldFrames{0};
  std::atomic<int64_t> m_codecHeldFrames{0};
  std::atomic<uint64_t> m_queuedBytes{0};

  //! \brief When the last block arrived, so an ongoing stall is visible.
  std::atomic<uint64_t> m_lastBlockUs{0};
  std::atomic<uint64_t> m_blockGapUs{0};

  std::atomic<uint64_t> m_fedUnits{0};
  std::atomic<uint64_t> m_fedBytes{0};
  std::atomic<uint64_t> m_refusedBank{0};
  std::atomic<uint64_t> m_refusedQueue{0};
  std::atomic<uint64_t> m_feedWaitUs{0};
  std::atomic<uint64_t> m_lastFeedUs{0};

  //! \brief Set by Mark and consumed by the sampler, which rebases on it.
  std::atomic<bool> m_rebase{false};

  std::atomic<uint64_t> m_pollCalls{0};
  std::atomic<uint64_t> m_pollTotalUs{0};
  std::atomic<uint64_t> m_pollMaxUs{0};
};
