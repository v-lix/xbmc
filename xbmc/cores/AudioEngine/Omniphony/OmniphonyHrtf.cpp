/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyHrtf.h"

#include "FileItem.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/LocalizeStrings.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace ActiveAE
{

namespace
{

// The profile's own, not the master profile's: a profile that chose no file
// would otherwise discard the copy another profile's choice put here. For the
// master profile the two are the same folder.
constexpr const char* HRTF_DIR = "special://profile/omniphony/";
constexpr const char* HRTF_FILE = "special://profile/omniphony/hrtf.sofa";
//! Written first, promoted only once it has been checked.
constexpr const char* HRTF_TEMP = "special://profile/omniphony/hrtf.sofa.part";
//! Where the staged copy came from - see StageIfChanged.
constexpr const char* HRTF_SOURCE = "special://profile/omniphony/hrtf.source";
//! The engine's finished grids of the staged copy, one per stream rate in
//! kHz - see GridCachePath.
constexpr const char* HRTF_GRIDS = "special://profile/omniphony/hrtf{khz}.grid";
//! The grid built when a file is chosen, before the file replaces the staged
//! one, so that a cancel leaves the previous file and its grids as they were.
constexpr const char* HRTF_GRID_NEXT = "special://profile/omniphony/hrtf.grid.next";

//! The grid of the staged copy for a stream at \p rate Hz.
std::string GridFor(unsigned int rate)
{
  std::string grid = HRTF_GRIDS;
  StringUtils::Replace(grid, "{khz}", std::to_string(rate / 1000));
  return grid;
}

/*!
 * \brief Every grid of a copy that is no longer the staged one, at every
 * rate, so that none is left behind for a file no longer in use. The engine
 * writes them, so the folder is listed afresh rather than from Kodi's cache.
 * hrtf.grid is the single grid of earlier builds. The grid of a file being
 * chosen (HRTF_GRID_NEXT) goes only with \p next.
 */
void DiscardGrids(bool next)
{
  CFileItemList items;
  XFILE::CDirectory::GetDirectory(HRTF_DIR, items, "",
                                  XFILE::DIR_FLAG_BYPASS_CACHE | XFILE::DIR_FLAG_NO_FILE_DIRS);
  for (const auto& item : items)
  {
    const std::string name = URIUtils::GetFileName(item->GetPath());
    std::string stem = name;
    if (StringUtils::EndsWith(stem, ".part"))
      stem.erase(stem.size() - 5);
    if (StringUtils::EndsWith(stem, ".next"))
    {
      if (!next)
        continue;
      stem.erase(stem.size() - 5);
    }
    if (StringUtils::StartsWith(stem, "hrtf") && StringUtils::EndsWith(stem, ".grid") &&
        std::all_of(stem.begin() + 4, stem.end() - 5,
                    [](char c) { return StringUtils::isasciidigit(c); }))
      XFILE::CFile::Delete(item->GetPath());
  }
}

/*!
 * \brief Have the engine build the chosen file's grid now, behind a busy
 * dialog, so that the first film plays the set from its start.
 *
 * What the first stream at COmniphonyHrtf::GRID_RATE would otherwise do while
 * the built-in set plays: the grid that stream reads, with diffuse-field
 * equalisation, stamped with the engine's build. Built from the checked copy
 * into HRTF_GRID_NEXT, before the copy replaces the staged file.
 *
 * \return Cancelled when the listener cancelled; Done when the grid is
 *         built; anything else leaves the building to the first film, as
 *         before.
 */
COmniphonyTool::Status PrepareGrid()
{
  XFILE::CFile::Delete(HRTF_GRID_NEXT);
  COmniphonyTool tool({"--prepare-hrtf", COmniphonyTool::EnginePath(),
                       CSpecialProtocol::TranslatePath(HRTF_TEMP),
                       CSpecialProtocol::TranslatePath(HRTF_GRID_NEXT),
                       std::to_string(COmniphonyHrtf::GRID_RATE), "1"},
                      {});
  COmniphonyTool::Status status = tool.Execute(true);
  if (status == COmniphonyTool::Status::Done && tool.Word() == "prepared")
  {
    CLog::Log(LOGINFO, "Omniphony: HRIR grid of the chosen HRTF: {}", tool.Detail());
    return status;
  }
  // A killed engine leaves its part behind; it writes nothing in place.
  XFILE::CFile::Delete(std::string(HRTF_GRID_NEXT) + ".part");
  XFILE::CFile::Delete(HRTF_GRID_NEXT);
  if (status == COmniphonyTool::Status::Cancelled)
    return status;
  CLog::Log(LOGWARNING, "Omniphony: HRIR grid not built now ({} {} {}), the first film builds it",
            static_cast<int>(status), tool.Reason(), tool.Detail());
  return COmniphonyTool::Status::Unavailable;
}

//! Every SOFA file is an HDF5 container, and every HDF5 file starts with this.
constexpr unsigned char HDF5_SIGNATURE[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};

/*!
 * Smallest plausible set of impulse responses. A real one is measured at
 * hundreds of directions; the smallest the engine's own test corpus carries is
 * about 20 kB. Anything under this is a truncated download or the wrong file
 * with the right extension.
 */
constexpr int64_t MIN_SIZE = 8 * 1024;

//! Read in pieces so a large file never has to be resident.
constexpr size_t CHUNK = 256 * 1024;

/*!
 * \brief Whether \a needle appears anywhere in the file.
 *
 * Both markers this looks for are stored as plain text: one is a dataset name
 * in the object header, the other an attribute value. Searching for them is
 * not a substitute for parsing HDF5, and does not pretend to be - it answers
 * the two questions that separate a usable HRIR file from every other file
 * with a .sofa extension, which is what the engine's reader will not tell us.
 * Chunks overlap by the needle length so a match cannot fall down the seam.
 */
bool Contains(XFILE::CFile& file, const char* needle)
{
  const size_t len = std::strlen(needle);
  if (len == 0 || len >= CHUNK)
    return false;

  if (file.Seek(0, SEEK_SET) < 0)
    return false;

  std::vector<char> buffer(CHUNK);
  size_t carry = 0;

  while (true)
  {
    const ssize_t got = file.Read(buffer.data() + carry, CHUNK - carry);
    if (got <= 0)
      return false;

    const size_t have = carry + static_cast<size_t>(got);
    if (std::search(buffer.begin(), buffer.begin() + have, needle, needle + len) !=
        buffer.begin() + have)
      return true;

    // Keep the last len-1 bytes: a marker may straddle two reads.
    carry = std::min(have, len - 1);
    std::memmove(buffer.data(), buffer.data() + have - carry, carry);
  }
}

} // unnamed namespace

COmniphonyHrtf::Result COmniphonyHrtf::Validate(const std::string& path,
                                                bool interactive,
                                                OmniphonySofaInfo& contents,
                                                bool& described)
{
  described = false;

  XFILE::CFile file;
  if (!file.Open(path))
    return Result::NotFound;

  if (file.GetLength() < MIN_SIZE)
    return Result::TooSmall;

  // The signature, then the superblock revision immediately after it. The
  // engine's reader handles revisions 0 to 3; 4 and later are a newer HDF5
  // than it knows, and it refuses them outright.
  unsigned char header[9] = {};
  if (file.Read(header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)))
    return Result::NotSofa;
  if (std::memcmp(header, HDF5_SIGNATURE, sizeof(HDF5_SIGNATURE)) != 0)
    return Result::NotSofa;
  if (header[8] > 3)
    return Result::Unreadable;

  // The engine's own answer where it can give one. "unusable" is its reader
  // refusing the layout, which is the Unreadable case above in a form the
  // header does not show; anything else it cannot say - no helper, an engine
  // from before it could describe - leaves the markers below to decide.
  std::string failure;
  switch (COmniphonyTool::Describe(path, interactive, contents, failure))
  {
    case COmniphonyTool::Status::Cancelled:
      return Result::Cancelled;
    case COmniphonyTool::Status::Done:
      if (failure.empty())
      {
        described = true;
        if (contents.hrtf)
          return Result::Ok;
        return contents.room ? Result::RoomResponse : Result::WrongConvention;
      }
      if (failure == "unusable")
        return Result::Unreadable;
      break;
    case COmniphonyTool::Status::Unavailable:
    case COmniphonyTool::Status::Unreadable:
      break;
  }

  if (!Contains(file, "Data.IR"))
    return Result::NoImpulseResponses;
  if (!Contains(file, "SimpleFreeFieldHRIR"))
    return Result::WrongConvention;

  return Result::Ok;
}

COmniphonyHrtf::Result COmniphonyHrtf::Stage(const std::string& path,
                                             bool interactive,
                                             OmniphonySofaInfo& contents,
                                             bool& described)
{
  described = false;
  if (path.empty())
  {
    Clear();
    return Result::Ok;
  }

  if (!XFILE::CDirectory::Exists(HRTF_DIR) && !XFILE::CDirectory::Create(HRTF_DIR))
  {
    CLog::Log(LOGERROR, "Omniphony: could not create {}", HRTF_DIR);
    return Result::CopyFailed;
  }

  // Copy first, check second. The chosen file may be on a share, and reading
  // it twice - once to check, once to copy - would read it twice over the
  // network and leave a window where it could change in between.
  XFILE::CFile::Delete(HRTF_TEMP);
  if (!XFILE::CFile::Copy(path, HRTF_TEMP))
  {
    CLog::Log(LOGERROR, "Omniphony: could not copy '{}' into the profile", path);
    return Result::CopyFailed;
  }

  const Result result = Validate(HRTF_TEMP, interactive, contents, described);
  if (result != Result::Ok)
  {
    XFILE::CFile::Delete(HRTF_TEMP);
    CLog::Log(LOGWARNING, "Omniphony: '{}' refused as an HRTF: {}", path, static_cast<int>(result));
    return result;
  }

  // Only when the listener chose the file: a stream opening that finds the
  // copy gone stages it again without a dialog, and builds as before. A
  // cancel here is a cancel of the choice: nothing has replaced the staged
  // file or its grids yet.
  bool gridBuilt = false;
  if (interactive)
  {
    const COmniphonyTool::Status grid = PrepareGrid();
    if (grid == COmniphonyTool::Status::Cancelled)
    {
      XFILE::CFile::Delete(HRTF_TEMP);
      CLog::Log(LOGINFO, "Omniphony: choosing '{}' as the HRTF was cancelled", path);
      return Result::Cancelled;
    }
    gridBuilt = grid == COmniphonyTool::Status::Done;
  }

  // Rename over the top rather than deleting first: on this platform that is
  // one atomic replace, so there is no instant at which the listener has no
  // HRTF at all, and a rename that fails leaves the previous file untouched.
  // Only if the replace is refused - a filesystem that will not rename onto an
  // existing name - is the older, lossy order worth trying.
  if (!XFILE::CFile::Rename(HRTF_TEMP, HRTF_FILE))
  {
    XFILE::CFile::Delete(HRTF_FILE);
    if (!XFILE::CFile::Rename(HRTF_TEMP, HRTF_FILE))
    {
      XFILE::CFile::Delete(HRTF_TEMP);
      XFILE::CFile::Delete(HRTF_GRID_NEXT);
      CLog::Log(LOGERROR, "Omniphony: could not put the checked HRTF in place");
      return Result::CopyFailed;
    }
  }

  // The grids of the file just replaced go; the one just built takes their
  // place, for its rate.
  DiscardGrids(false);
  if (gridBuilt && !XFILE::CFile::Rename(HRTF_GRID_NEXT, GridFor(GRID_RATE)))
    XFILE::CFile::Delete(HRTF_GRID_NEXT);

  XFILE::CFile note;
  if (note.OpenForWrite(HRTF_SOURCE, true))
  {
    note.Write(path.data(), path.size());
    note.Close();
  }

  CLog::Log(LOGINFO, "Omniphony: using the personal HRTF copied from '{}'", path);
  return Result::Ok;
}

COmniphonyHrtf::Result COmniphonyHrtf::StageIfChanged(const std::string& path)
{
  // What was staged last time, and from where. Both have to still hold: the
  // note without the file means the copy was removed underneath us.
  std::string was;
  if (IsStaged())
  {
    XFILE::CFile note;
    if (note.Open(HRTF_SOURCE))
    {
      char buf[1024] = {};
      const ssize_t got = note.Read(buf, sizeof(buf) - 1);
      if (got > 0)
        was.assign(buf, static_cast<size_t>(got));
    }
  }

  if (was == path)
    return Result::Ok;

  OmniphonySofaInfo contents;
  bool described = false;
  return Stage(path, false, contents, described);
}

void COmniphonyHrtf::Clear()
{
  XFILE::CFile::Delete(HRTF_TEMP);
  XFILE::CFile::Delete(HRTF_SOURCE);
  DiscardGrids(true);
  if (XFILE::CFile::Exists(HRTF_FILE))
  {
    XFILE::CFile::Delete(HRTF_FILE);
    CLog::Log(LOGINFO, "Omniphony: personal HRTF discarded, using the built-in set");
  }
}

std::string COmniphonyHrtf::StagedPath()
{
  if (!XFILE::CFile::Exists(HRTF_FILE))
    return {};
  return CSpecialProtocol::TranslatePath(HRTF_FILE);
}

std::string COmniphonyHrtf::GridCachePath()
{
  return CSpecialProtocol::TranslatePath(HRTF_GRIDS);
}

bool COmniphonyHrtf::GridKept(unsigned int rate)
{
  return XFILE::CFile::Exists(GridFor(rate), false);
}

bool COmniphonyHrtf::IsStaged()
{
  return XFILE::CFile::Exists(HRTF_FILE);
}

std::string COmniphonyHrtf::Explain(Result result)
{
  switch (result)
  {
    case Result::Ok:
      return g_localizeStrings.Get(39333);
    case Result::NotFound:
      return g_localizeStrings.Get(39334);
    case Result::TooSmall:
      return g_localizeStrings.Get(39335);
    case Result::NotSofa:
      return g_localizeStrings.Get(39336);
    case Result::Unreadable:
      return g_localizeStrings.Get(39337);
    case Result::NoImpulseResponses:
      return g_localizeStrings.Get(39338);
    case Result::WrongConvention:
      return g_localizeStrings.Get(39339);
    case Result::RoomResponse:
      return g_localizeStrings.Get(39354);
    case Result::CopyFailed:
      return g_localizeStrings.Get(39340);
    case Result::Cancelled:
      return {};
  }
  return {};
}

} // namespace ActiveAE
