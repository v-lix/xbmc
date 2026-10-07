/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyRoom.h"

#include "filesystem/Directory.h"
#include "filesystem/DirectoryCache.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/LocalizeStrings.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <cstring>

namespace ActiveAE
{

namespace
{

//! Created at boot by CoreELEC and shared over Samba - see Folder().
constexpr const char* STORAGE_FOLDER = "/storage/sofa/";

//! The copy that plays, beside the staged HRTF set - see StageIfChanged.
constexpr const char* STAGED_DIR = "special://profile/omniphony/";
constexpr const char* STAGED_ROOM = "special://profile/omniphony/brir.room";
//! Written first, promoted only once it has been checked.
constexpr const char* STAGED_PART = "special://profile/omniphony/brir.room.part";
//! The file the listener chose, which the copy was staged for.
constexpr const char* STAGED_SOURCE = "special://profile/omniphony/brir.source";

//! The first bytes of every prepared room; the engine tells a room from a SOFA
//! file by them, whatever the file is called.
constexpr char ROOM_MAGIC[8] = {'O', 'M', 'N', 'I', 'R', 'O', 'O', 'M'};
//! Where the header keeps the loudspeaker count: after the magic, the format
//! version and the rate, each a little-endian u32.
constexpr size_t ROOM_EMITTERS_AT = 16;

//! What a room is made from, as the room carries it: the file, its size and
//! its time, one per line - see SourceOf.
std::string Identity(const std::string& chosen, const struct __stat64& st)
{
  return chosen + "\n" + std::to_string(st.st_size) + "\n" + std::to_string(st.st_mtime) + "\n";
}

uint32_t LittleEndian(const unsigned char* p)
{
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

/*!
 * \brief What the prepared room at \p room says it was made from, or empty.
 *
 * Kodi hands the engine the text when it prepares a room, and the engine
 * keeps it in the room, after its conventions: the magic, five u32 words -
 * the last the conventions' length - the conventions, then a u32 length and
 * this text (see ABI.md). So a room copied to another box, or backed up and
 * brought back, still says what it is, and no note has to travel with it.
 */
std::string SourceOf(const std::string& room)
{
  XFILE::CFile file;
  unsigned char header[28] = {};
  if (!file.Open(room) ||
      file.Read(header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
      std::memcmp(header, ROOM_MAGIC, sizeof(ROOM_MAGIC)) != 0)
    return {};
  const uint32_t conventions = LittleEndian(header + 24);
  if (conventions > 256 || file.Seek(conventions, SEEK_CUR) < 0)
    return {};
  unsigned char length[4] = {};
  if (file.Read(length, sizeof(length)) != static_cast<ssize_t>(sizeof(length)))
    return {};
  const uint32_t n = LittleEndian(length);
  if (n > 4096)
    return {};
  std::string text(n, '\0');
  if (n && file.Read(text.data(), n) != static_cast<ssize_t>(n))
    return {};
  return text;
}

std::string ReadNote(const std::string& note)
{
  XFILE::CFile file;
  if (!file.Open(note))
    return {};
  char buf[4096] = {};
  const ssize_t got = file.Read(buf, sizeof(buf) - 1);
  return got > 0 ? std::string(buf, static_cast<size_t>(got)) : std::string();
}

void WriteNote(const std::string& note, const std::string& text)
{
  XFILE::CFile file;
  if (file.OpenForWrite(note, true))
  {
    file.Write(text.data(), text.size());
    file.Close();
  }
}

//! The prepared room a chosen file is kept as, in \p folder: its name with
//! the last extension, whatever it is, replaced. RemoveExtension would not do:
//! it strips only the extensions Kodi plays, which .sofa and .room are not.
std::string RoomFor(const std::string& folder, const std::string& chosen)
{
  return folder + URIUtils::ReplaceExtension(URIUtils::GetFileName(chosen), ".room");
}

//! Rename over the top, as COmniphonyHrtf::Stage does and for its reasons:
//! one atomic replace, and the previous room intact if it fails.
bool Promote(const std::string& part, const std::string& room)
{
  if (XFILE::CFile::Rename(part, room))
    return true;
  XFILE::CFile::Delete(room);
  if (XFILE::CFile::Rename(part, room))
    return true;
  XFILE::CFile::Delete(part);
  return false;
}

/*!
 * \brief Copy \p room into the profile as the room that plays, for \p chosen.
 *
 * The prepared rooms in the SOFA folder are the listener's to keep, rename,
 * share and delete; the one that plays is a copy, as the HRTF set is, so that
 * nothing done to that folder - over the network, from another box - can take
 * a room away from a film. Checked before it replaces the previous copy, which
 * survives a failure.
 */
bool Stage(const std::string& room, const std::string& chosen)
{
  if (!XFILE::CDirectory::Exists(STAGED_DIR) && !XFILE::CDirectory::Create(STAGED_DIR))
  {
    CLog::Log(LOGERROR, "COmniphonyRoom: could not create {}", STAGED_DIR);
    return false;
  }
  XFILE::CFile::Delete(STAGED_PART);
  if (!XFILE::CFile::Copy(room, STAGED_PART) || !COmniphonyRoom::IsPrepared(STAGED_PART) ||
      !Promote(STAGED_PART, STAGED_ROOM))
  {
    XFILE::CFile::Delete(STAGED_PART);
    CLog::Log(LOGERROR, "COmniphonyRoom: could not copy {} into the profile", room);
    return false;
  }
  WriteNote(STAGED_SOURCE, chosen);
  CLog::Log(LOGINFO, "COmniphonyRoom: using the room prepared from '{}'", chosen);
  return true;
}

//! The prepared room for \p chosen in the SOFA folder, as a Kodi path, or
//! empty when there is none or it was made from another file since.
std::string InFolder(const std::string& chosen)
{
  const std::string room = RoomFor(COmniphonyRoom::Folder(), chosen);
  if (!XFILE::CFile::Exists(room))
    return {};

  // A room chosen where it lies is its own answer, and so is one chosen
  // elsewhere, which was copied here under its own name. A room set has to
  // be what the room was made from.
  if (!URIUtils::PathEquals(chosen, room) && !URIUtils::HasExtension(chosen, ".room"))
  {
    const std::string source = SourceOf(room);
    if (source.substr(0, source.find('\n')) != chosen)
      return {};
  }
  return room;
}

/*!
 * \brief Put \p room in play for \p chosen, and say what it holds, for the
 * dialog that reports \p result.
 */
COmniphonyRoom::Outcome Staged(const std::string& room,
                               const std::string& chosen,
                               COmniphonyRoom::Result result)
{
  COmniphonyRoom::Outcome outcome;
  if (!Stage(room, chosen))
  {
    outcome.result = COmniphonyRoom::Result::WriteFailed;
    return outcome;
  }
  outcome.result = result;
  outcome.room = room;
  outcome.chosenRoom = COmniphonyRoom::IsPrepared(chosen);
  std::string failure;
  outcome.described = COmniphonyTool::Describe(STAGED_ROOM, true, outcome.contents, failure) ==
                          COmniphonyTool::Status::Done &&
                      failure.empty();
  return outcome;
}

} // unnamed namespace

std::string COmniphonyRoom::Folder()
{
  if (!XFILE::CDirectory::Exists(STORAGE_FOLDER) && !XFILE::CDirectory::Create(STORAGE_FOLDER))
    CLog::Log(LOGERROR, "COmniphonyRoom: could not create {}", STORAGE_FOLDER);
  return STORAGE_FOLDER;
}

bool COmniphonyRoom::IsPrepared(const std::string& path)
{
  XFILE::CFile file;
  char magic[sizeof(ROOM_MAGIC)] = {};
  return file.Open(path) &&
         file.Read(magic, sizeof(magic)) == static_cast<ssize_t>(sizeof(magic)) &&
         std::memcmp(magic, ROOM_MAGIC, sizeof(magic)) == 0;
}

unsigned int COmniphonyRoom::Loudspeakers(const std::string& room)
{
  XFILE::CFile file;
  unsigned char header[ROOM_EMITTERS_AT + 4] = {};
  if (!file.Open(room) ||
      file.Read(header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)) ||
      std::memcmp(header, ROOM_MAGIC, sizeof(ROOM_MAGIC)) != 0)
    return 0;
  return LittleEndian(header + ROOM_EMITTERS_AT);
}

COmniphonyRoom::Outcome COmniphonyRoom::Prepare(const std::string& chosen)
{
  Outcome outcome;

  struct __stat64 st = {};
  if (XFILE::CFile::Stat(chosen, &st) != 0)
  {
    outcome.result = Result::NotFound;
    return outcome;
  }

  const std::string room = RoomFor(Folder(), chosen);
  const std::string part = room + ".part";
  const std::string identity = Identity(chosen, st);

  // Already prepared: checked whole by the engine first - a room cut short in
  // a copy, or written by an engine of another format, is refused here rather
  // than staged to fail at the next film. Then used where it is in the folder,
  // and otherwise copied there - a few MB, and the engine cannot open a share.
  // An engine too old to describe leaves the header's word for it.
  if (IsPrepared(chosen))
  {
    std::string failure;
    switch (COmniphonyTool::Describe(chosen, true, outcome.contents, failure))
    {
      case COmniphonyTool::Status::Cancelled:
        outcome.result = Result::Cancelled;
        return outcome;
      case COmniphonyTool::Status::Done:
        if (failure == "unusable" || (failure.empty() && !outcome.contents.room))
        {
          outcome.result = Result::Damaged;
          return outcome;
        }
        break;
      case COmniphonyTool::Status::Unavailable:
      case COmniphonyTool::Status::Unreadable:
        break;
    }

    if (!URIUtils::PathEquals(chosen, room))
    {
      XFILE::CFile::Delete(part);
      if (!XFILE::CFile::Copy(chosen, part) || !Promote(part, room))
      {
        XFILE::CFile::Delete(part);
        outcome.result = Result::WriteFailed;
        return outcome;
      }
    }
    return Staged(room, chosen, Result::Ok);
  }

  // The same file as last time, unchanged, and its room still there: the
  // room says so itself. Its header can say so of a room damaged since, so
  // the engine reads it whole before it replaces the one in play; one it
  // cannot read is prepared again from the file. An engine too old to
  // describe leaves the header's word for it, as above.
  if (SourceOf(room) == identity)
  {
    std::string failure;
    OmniphonySofaInfo contents;
    switch (COmniphonyTool::Describe(room, true, contents, failure))
    {
      case COmniphonyTool::Status::Cancelled:
        outcome.result = Result::Cancelled;
        return outcome;
      case COmniphonyTool::Status::Done:
        if (failure == "unusable" || (failure.empty() && !contents.room))
        {
          CLog::Log(LOGWARNING, "COmniphonyRoom: {} is damaged - preparing '{}' again", room,
                    chosen);
          break;
        }
        return Staged(room, chosen, Result::Reused);
      case COmniphonyTool::Status::Unavailable:
      case COmniphonyTool::Status::Unreadable:
        return Staged(room, chosen, Result::Reused);
    }
  }

  COmniphonyTool tool({"--prepare-brir", COmniphonyTool::EnginePath(),
                       CSpecialProtocol::TranslatePath(room), identity},
                      chosen);
  switch (tool.Execute(true))
  {
    case COmniphonyTool::Status::Done:
      break;
    case COmniphonyTool::Status::Unavailable:
      outcome.result = Result::Unsupported;
      return outcome;
    case COmniphonyTool::Status::Unreadable:
      outcome.result = Result::NotFound;
      return outcome;
    case COmniphonyTool::Status::Cancelled:
      // The engine writes through <room>.part and renames only once it has
      // finished, so a room already there is untouched; only the part is left.
      XFILE::CFile::Delete(part);
      outcome.result = Result::Cancelled;
      return outcome;
  }

  if (tool.Word() == "prepared")
  {
    // Written by the helper, not through Kodi: a listing of the folder cached
    // by the file browser that chose the file does not have it, and opening a
    // file missing from a cached listing fails without looking.
    g_directoryCache.ClearDirectory(URIUtils::GetDirectory(room));
    CLog::Log(LOGINFO, "COmniphonyRoom: '{}' prepared as {}: {}", chosen, room, tool.Detail());
    return Staged(room, chosen, Result::Ok);
  }

  XFILE::CFile::Delete(part);
  outcome.detail = tool.Detail();
  CLog::Log(LOGWARNING, "COmniphonyRoom: '{}' not prepared: {} {}", chosen, tool.Reason(),
            tool.Detail());

  const std::string& reason = tool.Reason();
  if (reason == "unusable")
  {
    // Not a room the engine can prepare. What it is instead is the useful
    // answer - an HRTF set chosen here by mistake belongs under Custom - so it
    // is worth reading the file a second time to say.
    std::string failure;
    if (COmniphonyTool::Describe(chosen, true, outcome.contents, failure) ==
            COmniphonyTool::Status::Done &&
        failure.empty())
    {
      outcome.described = true;
      outcome.result = Result::NotARoom;
    }
    else
      outcome.result = failure == "unusable" ? Result::NotSofa : Result::EngineFailed;
  }
  else if (reason == "memory")
    outcome.result = Result::TooLarge;
  else if (reason == "input")
    outcome.result = Result::ReadFailed;
  else if (reason == "write")
    outcome.result = Result::WriteFailed;
  else if (reason == "unsupported" || reason == "engine")
    outcome.result = Result::Unsupported;
  else
    outcome.result = Result::EngineFailed;
  return outcome;
}

std::string COmniphonyRoom::StageIfChanged(const std::string& chosen)
{
  if (chosen.empty())
  {
    Clear();
    return {};
  }

  // Normally this: the room was staged when it was chosen.
  if (XFILE::CFile::Exists(STAGED_ROOM) && ReadNote(STAGED_SOURCE) == chosen)
    return CSpecialProtocol::TranslatePath(STAGED_ROOM);

  // A profile that has not staged it yet, or a copy since removed: the room
  // prepared in the SOFA folder will do, a few MB to copy. Nothing is prepared
  // here.
  const std::string room = InFolder(chosen);
  if (room.empty() || !Stage(room, chosen))
    return {};
  return CSpecialProtocol::TranslatePath(STAGED_ROOM);
}

void COmniphonyRoom::Clear()
{
  XFILE::CFile::Delete(STAGED_PART);
  XFILE::CFile::Delete(STAGED_SOURCE);
  if (XFILE::CFile::Exists(STAGED_ROOM))
  {
    XFILE::CFile::Delete(STAGED_ROOM);
    CLog::Log(LOGINFO, "COmniphonyRoom: room discarded from the profile");
  }
}

std::string COmniphonyRoom::Explain(const Outcome& outcome)
{
  std::string text;
  switch (outcome.result)
  {
    case Result::Ok:
      text = g_localizeStrings.Get(39356);
      break;
    case Result::Reused:
      text = g_localizeStrings.Get(39357);
      break;
    case Result::NotFound:
      text = g_localizeStrings.Get(39334);
      break;
    case Result::NotSofa:
      text = g_localizeStrings.Get(39336);
      break;
    case Result::Damaged:
      text = g_localizeStrings.Get(39367);
      break;
    case Result::NotARoom:
      // A file neither stage takes is said to be so by its description.
      if (outcome.contents.hrtf)
        text = g_localizeStrings.Get(39355);
      break;
    case Result::TooLarge:
      text = g_localizeStrings.Get(39358);
      break;
    case Result::ReadFailed:
      text = g_localizeStrings.Get(39359);
      break;
    case Result::WriteFailed:
      text = g_localizeStrings.Get(39360);
      break;
    case Result::Unsupported:
      text = g_localizeStrings.Get(39361);
      break;
    case Result::EngineFailed:
      text = g_localizeStrings.Get(39362);
      break;
    case Result::Cancelled:
      return {};
  }

  // What the prepared room is, which the listener keeps: the .room file made
  // here, which is all a room needs from now on, so the room set it was made
  // from can go; or the .room chosen, which travels on its own.
  if ((outcome.result == Result::Ok || outcome.result == Result::Reused) && !outcome.room.empty())
    text +=
        "[CR][CR]" + StringUtils::Format(g_localizeStrings.Get(outcome.chosenRoom ? 39369 : 39368),
                                         URIUtils::GetFileName(outcome.room));
  if (outcome.described)
    text += (text.empty() ? "" : "[CR][CR]") + OmniphonyDescribeSofaInfo(outcome.contents);
  // The engine's numbers say how far short the memory was, and its words what
  // failed; English, but more use than nothing to someone reading them out.
  if ((outcome.result == Result::TooLarge || outcome.result == Result::EngineFailed) &&
      !outcome.detail.empty())
    text += "[CR][CR]" + outcome.detail;
  return text;
}

} // namespace ActiveAE
