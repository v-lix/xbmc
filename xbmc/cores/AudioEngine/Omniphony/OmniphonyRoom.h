/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
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

/*!
 * \brief A measured listening room (BRIR) the listener has chosen.
 *
 * A room set is the responses at a dummy head's ears from every loudspeaker
 * of a real array, at every head orientation measured: the BBC's 7.1.4 room
 * is 274 MB. Without head tracking, which Kodi has none of, a session renders
 * only the orientation nearest straight ahead, so the set is prepared once,
 * when it is chosen, into a file of that orientation alone: a few MB that
 * loads in milliseconds and that the engine builds its virtual loudspeakers
 * from (orender_brir_prepare). The setting keeps the file the listener chose;
 * the prepared room is kept in Folder(), and a copy of it in the profile,
 * brir.room beside the staged HRTF set, is what the engine is given.
 *
 * Prepared rooms are named after what they came from - bbcrdlr_systemG.sofa
 * becomes bbcrdlr_systemG.room - and each carries, inside it, which file, of
 * what size and time, it was made from. That is how choosing the same file
 * again costs nothing, and how a room since made from another file of the
 * same name is not taken for it; and because the room carries it, the room
 * can be backed up, copied to another box and brought back on its own.
 */
class COmniphonyRoom
{
public:
  enum class Result
  {
    Ok, //!< prepared, or a prepared room copied into Folder()
    Reused, //!< prepared before from this same file, and kept
    NotFound, //!< the chosen file could not be opened
    NotSofa, //!< neither a SOFA file nor a prepared room
    Damaged, //!< a prepared room the engine cannot read whole
    NotARoom, //!< a SOFA file, but no room: see Outcome::contents
    TooLarge, //!< not enough free memory to prepare it
    ReadFailed, //!< the file ended before its size said it would
    WriteFailed, //!< the prepared room could not be saved
    Unsupported, //!< no helper here, or an engine that cannot prepare rooms
    EngineFailed, //!< the engine failed: see Outcome::detail
    Cancelled,
  };

  struct Outcome
  {
    Result result{Result::EngineFailed};
    //! What the prepared room holds, or for NotARoom what the file holds.
    OmniphonySofaInfo contents;
    bool described{false};
    //! The helper's own words for a failure, in English, for the log.
    std::string detail;
    //! For Ok and Reused: the prepared room in Folder(), and whether the
    //! file chosen was itself one rather than the SOFA file it is made from.
    std::string room;
    bool chosenRoom{false};
  };

  /*!
   * \brief Where prepared rooms are kept, as a Kodi path ending in a slash.
   *
   * /storage/sofa, which CoreELEC creates at boot and shares over Samba as
   * SOFA, so that room sets can be copied straight to the box and the
   * prepared rooms copied off it again; created here should it be missing.
   * The settings' file browsers open here.
   */
  static std::string Folder();

  /*!
   * \brief Make the file the listener chose renderable.
   *
   * A SOFA file is prepared into Folder() behind a cancellable busy dialog,
   * unless the room already there was prepared from it. A prepared room is
   * used where it is in Folder(), and copied there from anywhere else. Either
   * way the room is then copied into the profile, as the one that plays, and
   * described, for the dialog that reports it.
   *
   * Only from the GUI thread: it shows a dialog.
   */
  static Outcome Prepare(const std::string& chosen);

  /*!
   * \brief The room that plays for a chosen file, translated for the engine.
   *
   * The profile's copy, staged when the file was chosen; failing that - a
   * profile that has not staged it, a copy that was removed - the room
   * prepared from it in Folder(), staged now. Empty when there is neither.
   * Never prepares anything: that takes as long as a film's opening can wait,
   * and only the settings screen can show it. An empty \p chosen clears the
   * copy, as leaving the room does.
   */
  static std::string StageIfChanged(const std::string& chosen);

  //! \brief Discard the profile's copy. The rooms in Folder() are the
  //! listener's and stay.
  static void Clear();

  /*!
   * \brief Loudspeakers in a prepared room, from its header; 0 when \p room
   * is not one. A local path.
   */
  static unsigned int Loudspeakers(const std::string& room);

  //! \brief Localised account of an outcome, with what the file holds.
  static std::string Explain(const Outcome& outcome);

  //! \brief Whether the bytes at \p path start as a prepared room's do.
  static bool IsPrepared(const std::string& path);
};

} // namespace ActiveAE
