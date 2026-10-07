/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "OmniphonyTool.h"

#include <string>

namespace ActiveAE
{

//! \brief Which binaural response the render uses. Order matches settings.xml.
enum OmniphonyHrtfMode
{
  OMNI_HRTF_BUILTIN = 0,
  OMNI_HRTF_PERSONAL = 1, //!< "Custom (HRIR)": a SOFA HRTF set of the listener's
  OMNI_HRTF_ROOM = 2, //!< "Room (BRIR)": a measured room - see COmniphonyRoom
};

/*!
 * \brief The listener's own HRTF measurement, if they have chosen one.
 *
 * A head-related transfer function describes how a particular head and pair
 * of ears colour sound arriving from each direction. The engine ships one
 * measured set and can be pointed at another, supplied as a SOFA file.
 *
 * Kodi keeps exactly one such file, copied into the user profile, and treats
 * its presence as the whole of the answer to "is a personal HRTF in use". That
 * has three consequences worth stating, because they are the reason this class
 * exists rather than the setting being handed straight to the engine:
 *
 *  - The engine opens the path with the C library, so it cannot read a Kodi
 *    virtual path. A file chosen from a network share has to be copied before
 *    it can be used at all.
 *  - The copy is checked before it is kept. The engine's reader accepts a
 *    file whose structure it recognises and only warns about the rest, so a
 *    file of the wrong kind can load and render nonsense rather than failing.
 *  - Once staged, the file is local and its path is fixed, so nothing later in
 *    playback depends on a share still being mounted.
 */
class COmniphonyHrtf
{
public:
  //! The stream rate the staged set's grid is built for when the file is
  //! chosen - see GridCachePath.
  static constexpr unsigned int GRID_RATE = 48000;

  //! \brief Why a file was refused, or Ok.
  enum class Result
  {
    Ok,
    NotFound, //!< the file could not be opened
    TooSmall, //!< far too small to hold a set of impulse responses
    NotSofa, //!< not an HDF5 container, which every SOFA file is
    Unreadable, //!< an HDF5 layout this engine's reader does not accept
    NoImpulseResponses, //!< no Data.IR: frequency domain or filter coefficients
    WrongConvention, //!< neither stage takes it, or (unread) not SimpleFreeFieldHRIR
    RoomResponse, //!< a measured room, which belongs under Room (BRIR)
    CopyFailed, //!< could not be copied into the profile
    Cancelled, //!< the listener stopped the check
  };

  /*!
   * \brief Check a file the way the engine's reader will.
   *
   * The container is screened here first - big enough, an HDF5 file, a
   * revision the reader can parse - and then the engine itself is asked what
   * the file holds (COmniphonyTool::Describe): a set of directions the HRTF
   * stage takes, a room it does not, or neither. Asking is what tells a room
   * set from an HRTF set, which share a container and can share a convention,
   * and what lets the dialog say what was found. Where the engine cannot be
   * asked, two markers are looked for instead: time-domain impulse responses
   * and the free-field HRIR convention. A file that passes will load; a file
   * that fails would either be refused or, worse, accepted and rendered as
   * noise - a room cut to the HRTF stage's few milliseconds is no room.
   *
   * \param path        A Kodi path. Reading is sequential, so a large file on
   *                    a slow share is slow to check - prefer the local copy.
   * \param interactive the engine is asked behind a cancellable busy dialog
   * \param contents    what the engine found, when \p described
   */
  static Result Validate(const std::string& path,
                         bool interactive,
                         OmniphonySofaInfo& contents,
                         bool& described);

  /*!
   * \brief Copy a chosen file into the profile, replacing any previous one.
   *
   * The copy is validated before it replaces what is already there, so a bad
   * choice costs the user nothing: the previous file, if any, survives. A note
   * beside the copy records where it came from - see StageIfChanged. When
   * \p interactive, the engine builds the copy's grid for GRID_RATE behind
   * a busy dialog before it replaces anything - see GridCachePath - and a
   * cancel there cancels the choice, the previous file and its grids left
   * as they were.
   *
   * \return Ok when the file is staged and in use from the next stream on,
   *         Cancelled when the listener cancelled either busy dialog.
   */
  static Result Stage(const std::string& path,
                      bool interactive,
                      OmniphonySofaInfo& contents,
                      bool& described);

  /*!
   * \brief Stage \p path only if it is not what is already staged.
   *
   * Every stream open asks for the same file, and copying it each time would
   * fetch a measurement kept on a share once per film. The note beside the
   * copy records where it came from, so the usual answer is to do nothing:
   * the file was staged when it was chosen. Never interactive - a stream
   * opening asks this.
   */
  static Result StageIfChanged(const std::string& path);

  //! \brief Discard the staged file and return to the engine's own set.
  static void Clear();

  /*!
   * \brief Absolute path of the staged file, or empty when there is none.
   *
   * Translated out of special:// because it is handed to the engine, which
   * knows nothing about Kodi paths.
   */
  static std::string StagedPath();

  /*!
   * \brief Absolute path of the files the engine keeps the staged set's
   * finished HRIR grids in, beside it: hrtf{khz}.grid, one per stream rate
   * in kHz (hrtf48.grid, hrtf44.grid, hrtf96.grid ...).
   *
   * For streams with diffuse-field equalisation, which is how this codec
   * configures them. The GRID_RATE one is built when the listener chooses
   * the file (Stage); a stream at another rate builds its own at its first
   * film and keeps it, and every later stream at a rate reads its grid, so
   * the set plays from its start rather than after the seconds its grid
   * takes to build. A grid missing, or built by another engine build, is
   * built again by the next stream at its rate, so an updated engine
   * rebuilds each once. All of them are discarded with the staged copy, and
   * when another file is staged, so none is left for a file not in use.
   */
  static std::string GridCachePath();

  //! \brief Whether the staged set's grid for streams at \p rate Hz is kept.
  static bool GridKept(unsigned int rate);

  /*!
   * \brief Whether a personal file is staged in the profile.
   *
   * A fact about the copy, and only about the copy: whether the engine could
   * load it is the engine's to say, and the player reports that from what the
   * engine says it is convolving with - see CDVDAudioCodecOmniphony.
   */
  static bool IsStaged();

  //! \brief Localised, user-facing explanation of a result.
  static std::string Explain(Result result);
};

} // namespace ActiveAE
