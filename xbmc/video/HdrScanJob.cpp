/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HdrScanJob.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDFileInfo.h"
#include "dbwrappers/dataset.h"
#include "dialogs/GUIDialogBusy.h"
#include "filesystem/File.h"
#include "filesystem/MultiPathDirectory.h"
#include "filesystem/StackDirectory.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/IRunnable.h"
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/XMLUtils.h"
#include "utils/log.h"
#include "video/VideoDatabase.h"
#include "video/VideoInfoScanner.h"
#include "video/tags/VideoTagLoaderNFO.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::string ComparablePath(const std::string& path)
{
  if (URIUtils::IsStack(path))
    return XFILE::CStackDirectory::GetFirstStackedFile(path);

  return path;
}

/*! \brief The scope as a list of real paths.

 A video source that was set up with several folders is offered by the file browser as a single
 multipath entry, so an unexpanded scope would match none of the library paths behind it.
 */
std::vector<std::string> ExpandScope(const std::string& scope, bool scopeIsFolder)
{
  std::vector<std::string> scopes;
  if (scopeIsFolder && URIUtils::IsMultiPath(scope) &&
      XFILE::CMultiPathDirectory::GetPaths(scope, scopes) && !scopes.empty())
  {
    return scopes;
  }

  scopes.assign(1, scope);
  return scopes;
}

bool IsInScope(const std::string& path, const std::vector<std::string>& scopes, bool scopeIsFolder)
{
  const std::string comparablePath = ComparablePath(path);
  for (const auto& scope : scopes)
  {
    if (!scopeIsFolder)
    {
      if (URIUtils::PathEquals(comparablePath, scope, true))
        return true;

      continue;
    }

    std::string folder = scope;
    URIUtils::AddSlashAtEnd(folder);
    if (URIUtils::PathHasParent(comparablePath, folder) ||
        URIUtils::PathEquals(URIUtils::GetDirectory(comparablePath), folder, true))
    {
      return true;
    }
  }

  return false;
}

class CHdrScanDatabase final : public CVideoDatabase
{
public:
  bool GetCandidates(const std::string& scope,
                     bool scopeIsFolder,
                     std::vector<CHdrScanJob::Candidate>& candidates)
  {
    candidates.clear();
    std::set<int> seenFiles;

    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
    const bool scanHdr10Plus =
        settings && settings->GetBool(CSettings::SETTING_MYVIDEOS_EXTRACTHDR10PLUS);

    const std::vector<std::string> scopes = ExpandScope(scope, scopeIsFolder);

    try
    {
      const std::string query =
          "SELECT files.idFile, path.strPath, files.strFilename, streamdetails.strHdrType, "
          "streamdetails.strDvProfile "
          "FROM files JOIN path ON path.idPath = files.idPath "
          "JOIN streamdetails ON streamdetails.idFile = files.idFile "
          "WHERE streamdetails.iStreamType = %i";
      m_pDS->query(PrepareSQL(query.c_str(), static_cast<int>(CStreamDetail::VIDEO)));

      while (!m_pDS->eof())
      {
        const int idFile = m_pDS->fv(0).get_asInt();
        const std::string filePath =
            URIUtils::AddFileToFolder(m_pDS->fv(1).get_asString(), m_pDS->fv(2).get_asString());
        const std::string hdrType = m_pDS->fv(3).get_asString();
        const std::string dvProfile = m_pDS->fv(4).get_asString();

        // These columns are ours alone - nothing but this build's scanner writes them, and it
        // writes the whole set in a single pass. A Dolby Vision row without a profile is
        // therefore one written before that scanner existed, and the profile is enough to tell
        // the two apart. An empty strHdrTypeAlt is no help here: it is the normal state of most
        // Dolby Vision files, so testing it would make every entry eligible on every run.
        const bool needsDolbyVisionScan =
            StringUtils::EqualsNoCase(hdrType, "dolbyvision") && dvProfile.empty();
        // Nothing records that an HDR10 file has already been probed for HDR10+, so while the
        // setting is on every HDR10 row is eligible and a second run repeats the work.
        const bool needsHdr10Scan = scanHdr10Plus && StringUtils::EqualsNoCase(hdrType, "hdr10");

        if ((needsDolbyVisionScan || needsHdr10Scan) &&
            IsInScope(filePath, scopes, scopeIsFolder) && seenFiles.insert(idFile).second)
        {
          candidates.push_back({idFile, filePath});
        }

        m_pDS->next();
      }
      m_pDS->close();
      return true;
    }
    catch (...)
    {
      m_pDS->close();
      CLog::Log(LOGERROR, "{} failed while finding HDR scan candidates", __FUNCTION__);
      return false;
    }
  }

  /*! \brief Write the freshly read HDR fields into the file's video stream rows.
   \param idFile the library file id
   \param details the stream details just read from the file
   \param updatedStreams filled with the 1-based indices of the streams that were written
   \return true if at least one row was updated
   */
  bool UpdateHdrDetails(int idFile, const CStreamDetails& details, std::vector<int>& updatedStreams)
  {
    updatedStreams.clear();
    if (idFile < 0 || details.GetVideoStreamCount() == 0)
      return false;

    try
    {
      int databaseVideoStreams = 0;
      m_pDS->query(PrepareSQL("SELECT COUNT(*) FROM streamdetails WHERE idFile = %i AND "
                              "iStreamType = %i",
                              idFile, static_cast<int>(CStreamDetail::VIDEO)));
      if (!m_pDS->eof())
        databaseVideoStreams = m_pDS->fv(0).get_asInt();
      m_pDS->close();

      if (databaseVideoStreams <= 0)
        return false;

      // A single row can only be paired off with a single scanned stream without looking any
      // further. A file with several video streams whose entry holds one row - the shape a
      // third-party NFO tends to leave behind - says nothing about which of them that row
      // describes, so it goes through the matching below instead of being overwritten blindly.
      if (databaseVideoStreams == 1 && details.GetVideoStreamCount() == 1)
      {
        m_pDS->exec(PrepareSQL(
            "UPDATE streamdetails SET strHdrType = '%s', strHdrTypeAlt = '%s', "
            "strDvProfile = '%s' WHERE idFile = %i AND iStreamType = %i",
            details.GetVideoHdrType(1).c_str(), details.GetVideoHdrTypeAlt(1).c_str(),
            details.GetVideoDvProfile(1).c_str(), idFile, static_cast<int>(CStreamDetail::VIDEO)));
        updatedStreams.push_back(1);
        return true;
      }

      for (int stream = 1; stream <= details.GetVideoStreamCount(); ++stream)
      {
        const std::string codec = details.GetVideoCodec(stream);
        const int width = details.GetVideoWidth(stream);
        const int height = details.GetVideoHeight(stream);

        int matchingScannedStreams = 0;
        for (int other = 1; other <= details.GetVideoStreamCount(); ++other)
        {
          if (StringUtils::EqualsNoCase(codec, details.GetVideoCodec(other)) &&
              width == details.GetVideoWidth(other) && height == details.GetVideoHeight(other))
          {
            ++matchingScannedStreams;
          }
        }

        if (matchingScannedStreams != 1)
        {
          CLog::Log(LOGWARNING,
                    "{}: Cannot uniquely match scanned video stream {} for file id {}; skipping it",
                    __FUNCTION__, stream, idFile);
          continue;
        }

        int matchingDatabaseStreams = 0;
        m_pDS->query(PrepareSQL(
            "SELECT COUNT(*) FROM streamdetails WHERE idFile = %i AND iStreamType = %i AND "
            "strVideoCodec = '%s' AND iVideoWidth = %i AND iVideoHeight = %i",
            idFile, static_cast<int>(CStreamDetail::VIDEO), codec.c_str(), width, height));
        if (!m_pDS->eof())
          matchingDatabaseStreams = m_pDS->fv(0).get_asInt();
        m_pDS->close();

        if (matchingDatabaseStreams != 1)
        {
          CLog::Log(
              LOGWARNING,
              "{}: Cannot uniquely match database video stream {} for file id {}; skipping it",
              __FUNCTION__, stream, idFile);
          continue;
        }

        m_pDS->exec(PrepareSQL(
            "UPDATE streamdetails SET strHdrType = '%s', strHdrTypeAlt = '%s', "
            "strDvProfile = '%s' WHERE idFile = %i AND iStreamType = %i AND "
            "strVideoCodec = '%s' AND iVideoWidth = %i AND iVideoHeight = %i",
            details.GetVideoHdrType(stream).c_str(), details.GetVideoHdrTypeAlt(stream).c_str(),
            details.GetVideoDvProfile(stream).c_str(), idFile,
            static_cast<int>(CStreamDetail::VIDEO), codec.c_str(), width, height));
        updatedStreams.push_back(stream);
      }

      return !updatedStreams.empty();
    }
    catch (...)
    {
      m_pDS->close();
      updatedStreams.clear();
      CLog::Log(LOGERROR, "{} failed for file id {}", __FUNCTION__, idFile);
      return false;
    }
  }
};

class CHdrNfoFinder final : public CVideoTagLoaderNFO
{
public:
  using CVideoTagLoaderNFO::CVideoTagLoaderNFO;

  //! \brief The path the base constructor already resolved; no second lookup needed.
  const std::string& Path() const { return m_path; }
};

std::string FindNfoPath(CHdrScanDatabase& database, const std::string& filePath)
{
  VIDEO::SScanSettings scanSettings;
  const std::string lookupPath = URIUtils::GetDirectory(ComparablePath(filePath));
  ADDON::ScraperPtr scraper = database.GetScraperForPath(lookupPath, scanSettings);

  CFileItem item(filePath, false);
  const CHdrNfoFinder finder(item, scraper, scanSettings.parent_name);
  return finder.Path();
}

void SetOrRemoveString(TiXmlElement* parent, const char* name, const std::string& value)
{
  if (!parent)
    return;

  // XMLUtils::SetString() appends rather than replaces, so any existing entry has to be
  // removed first - otherwise the old value stays ahead of the new one and wins on read.
  TiXmlElement* child = parent->FirstChildElement(name);
  while (child)
  {
    TiXmlElement* next = child->NextSiblingElement(name);
    parent->RemoveChild(child);
    child = next;
  }

  if (!value.empty())
    XMLUtils::SetString(parent, name, value);
}

//! \brief The <video> children of the entry's <streamdetails>, creating nothing.
std::vector<TiXmlElement*> GetVideoNodes(TiXmlElement* root)
{
  std::vector<TiXmlElement*> videos;

  TiXmlElement* fileInfo = root ? root->FirstChildElement("fileinfo") : nullptr;
  TiXmlElement* streamDetails = fileInfo ? fileInfo->FirstChildElement("streamdetails") : nullptr;
  for (TiXmlElement* video = streamDetails ? streamDetails->FirstChildElement("video") : nullptr;
       video; video = video->NextSiblingElement("video"))
  {
    videos.push_back(video);
  }

  return videos;
}

/*! \brief Whether an NFO video node and a scanned stream describe the same video.

 Only the fields the node actually states are compared, so a node that says nothing about itself
 is taken at its position.
 */
bool DescribesSameVideo(const TiXmlElement* video, const CStreamDetails& details, int stream)
{
  std::string codec;
  if (XMLUtils::GetString(video, "codec", codec) && !codec.empty() &&
      !StringUtils::EqualsNoCase(codec, details.GetVideoCodec(stream)))
  {
    return false;
  }

  int width = 0;
  if (XMLUtils::GetInt(video, "width", width) && width > 0 &&
      width != details.GetVideoWidth(stream))
  {
    return false;
  }

  int height = 0;
  if (XMLUtils::GetInt(video, "height", height) && height > 0 &&
      height != details.GetVideoHeight(stream))
  {
    return false;
  }

  return true;
}

enum class NfoPatchResult
{
  UPDATED, //!< the NFO carried HDR entries for these streams and they were rewritten
  SKIPPED, //!< there was nothing to rewrite, or it could not be matched up safely
  FAILED, //!< the NFO should have been rewritten but could not be
};

NfoPatchResult PatchNfo(const std::string& nfoPath,
                        const CStreamDetails& details,
                        const std::vector<int>& streams)
{
  if (nfoPath.empty() || streams.empty())
    return NfoPatchResult::SKIPPED;

  CXBMCTinyXML document;
  if (!document.LoadFile(nfoPath))
  {
    CLog::Log(LOGWARNING, "{}: Unable to read NFO {}", __FUNCTION__, nfoPath);
    return NfoPatchResult::FAILED;
  }

  bool patched = false;
  for (TiXmlElement* root = document.RootElement(); root; root = root->NextSiblingElement())
  {
    // Never add <fileinfo>, <streamdetails> or <video> to somebody else's NFO. A video node
    // holding nothing but HDR entries is read back as the whole truth about that stream, which
    // would cost the entry its codec, resolution and duration. Existing nodes are rewritten, a
    // missing section is left alone, and the database update stands on its own either way.
    const std::vector<TiXmlElement*> videos = GetVideoNodes(root);
    if (videos.empty())
      continue;

    // Without one node per stream there is no way to tell which node belongs to which stream,
    // and guessing would write the wrong profile into a perfectly good file.
    if (static_cast<int>(videos.size()) != details.GetVideoStreamCount())
    {
      CLog::Log(LOGINFO,
                "{}: {} lists {} video stream(s) but the file has {}; leaving its HDR entries "
                "alone",
                __FUNCTION__, nfoPath, videos.size(), details.GetVideoStreamCount());
      continue;
    }

    bool linesUp = true;
    for (int stream = 1; stream <= details.GetVideoStreamCount() && linesUp; ++stream)
      linesUp = DescribesSameVideo(videos[stream - 1], details, stream);

    if (!linesUp)
    {
      CLog::Log(LOGINFO,
                "{}: the video streams listed in {} do not line up with the file; leaving its "
                "HDR entries alone",
                __FUNCTION__, nfoPath);
      continue;
    }

    for (const int stream : streams)
    {
      TiXmlElement* video = videos[stream - 1];
      SetOrRemoveString(video, "hdrtype", details.GetVideoHdrType(stream));
      SetOrRemoveString(video, "hdrtypealt", details.GetVideoHdrTypeAlt(stream));
      SetOrRemoveString(video, "dvprofile", details.GetVideoDvProfile(stream));
      patched = true;
    }
  }

  if (!patched)
    return NfoPatchResult::SKIPPED;

  const std::string backupPath = nfoPath + ".hdrscan.bak";
  XFILE::CFile::Delete(backupPath);
  if (!XFILE::CFile::Copy(nfoPath, backupPath))
  {
    CLog::Log(LOGWARNING, "{}: Unable to create NFO backup {}", __FUNCTION__, backupPath);
    return NfoPatchResult::FAILED;
  }

  if (document.SaveFile(nfoPath))
  {
    XFILE::CFile::Delete(backupPath);
    return NfoPatchResult::UPDATED;
  }

  CLog::Log(LOGWARNING, "{}: Unable to update NFO {}; restoring backup", __FUNCTION__, nfoPath);
  XFILE::CFile::Delete(nfoPath);
  if (XFILE::CFile::Copy(backupPath, nfoPath))
  {
    XFILE::CFile::Delete(backupPath);
  }
  else
  {
    CLog::Log(LOGERROR, "{}: Unable to restore NFO {}; backup remains at {}", __FUNCTION__, nfoPath,
              backupPath);
  }

  return NfoPatchResult::FAILED;
}

//! \brief Searches the library for candidates without blocking the GUI thread.
class CHdrScanCollector final : public IRunnable
{
public:
  CHdrScanCollector(std::string scope, bool scopeIsFolder, CHdrScanJob::Collection& collection)
    : m_scope(std::move(scope)), m_scopeIsFolder(scopeIsFolder), m_collection(collection)
  {
  }

  void Run() override
  {
    CHdrScanDatabase database;
    if (!database.Open())
    {
      CLog::Log(LOGERROR, "{}: Unable to open the video database", __FUNCTION__);
      return;
    }

    m_collection.queried =
        database.GetCandidates(m_scope, m_scopeIsFolder, m_collection.candidates);

    // Looking for the NFO of a candidate reaches out to the file system, so this is the part
    // that is worth running off-thread and worth being able to abandon.
    if (m_collection.queried)
    {
      for (const auto& candidate : m_collection.candidates)
      {
        if (m_cancelled)
          break;

        if (!FindNfoPath(database, candidate.path).empty())
        {
          m_collection.hasNfoFiles = true;
          break;
        }
      }
    }

    database.Close();
  }

  void Cancel() override { m_cancelled = true; }

private:
  const std::string m_scope;
  const bool m_scopeIsFolder;
  CHdrScanJob::Collection& m_collection;
  std::atomic<bool> m_cancelled{false};
};
} // unnamed namespace

CHdrScanJob::CHdrScanJob(std::vector<Candidate> candidates, bool updateNfo)
  : m_candidates(std::move(candidates)), m_updateNfo(updateNfo)
{
}

CHdrScanJob::Collection CHdrScanJob::Collect(const std::string& scope, bool scopeIsFolder)
{
  Collection collection;

  CHdrScanCollector collector(scope, scopeIsFolder, collection);
  // Wait() joins the worker before it returns, cancelled or not, so the collection is settled
  // by the time it is read here.
  collection.cancelled = !CGUIDialogBusy::Wait(&collector, 100, true);

  return collection;
}

bool CHdrScanJob::DoWork()
{
  CHdrScanDatabase database;
  if (!database.Open())
  {
    CLog::Log(LOGERROR, "{}: Unable to open the video database", __FUNCTION__);
    ++m_failed;
    return false;
  }

  SetTitle(g_localizeStrings.Get(37007));

  const std::vector<Candidate>& candidates = m_candidates;
  std::vector<int> updatedStreams;
  for (std::size_t index = 0; index < candidates.size(); ++index)
  {
    if (IsCancelled())
    {
      m_cancelled = true;
      break;
    }

    const Candidate& candidate = candidates[index];
    SetText(candidate.path);
    SetProgress(static_cast<int>(index), static_cast<int>(candidates.size()));

    CFileItem item(candidate.path, false);
    if (!CDVDFileInfo::GetFileStreamDetails(&item))
    {
      CLog::Log(LOGWARNING, "{}: Unable to scan {}", __FUNCTION__, candidate.path);
      ++m_failed;
      continue;
    }

    ++m_scanned;
    const CStreamDetails& details = item.GetVideoInfoTag()->m_streamDetails;
    if (!database.UpdateHdrDetails(candidate.idFile, details, updatedStreams))
    {
      CLog::Log(LOGWARNING, "{}: Unable to store the HDR details of {}", __FUNCTION__,
                candidate.path);
      ++m_failed;
      continue;
    }

    ++m_databaseUpdated;

    if (m_updateNfo)
    {
      const std::string nfoPath = FindNfoPath(database, candidate.path);
      if (!nfoPath.empty())
      {
        switch (PatchNfo(nfoPath, details, updatedStreams))
        {
          case NfoPatchResult::UPDATED:
            ++m_nfoUpdated;
            break;
          case NfoPatchResult::FAILED:
            CLog::Log(LOGWARNING, "{}: Unable to update the NFO of {}", __FUNCTION__,
                      candidate.path);
            ++m_failed;
            break;
          case NfoPatchResult::SKIPPED:
            break;
        }
      }
    }
  }

  if (candidates.empty())
    SetProgress(100.0f);
  else
    SetProgress(static_cast<int>(candidates.size()), static_cast<int>(candidates.size()));

  database.Close();
  return !m_cancelled;
}

bool CHdrScanJob::operator==(const CJob* job) const
{
  if (!job || StringUtils::CompareNoCase(job->GetType(), GetType()) != 0)
    return false;

  const auto* hdrScanJob = dynamic_cast<const CHdrScanJob*>(job);
  if (!hdrScanJob || m_updateNfo != hdrScanJob->m_updateNfo ||
      m_candidates.size() != hdrScanJob->m_candidates.size())
    return false;

  for (std::size_t index = 0; index < m_candidates.size(); ++index)
  {
    if (m_candidates[index].idFile != hdrScanJob->m_candidates[index].idFile)
      return false;
  }

  return true;
}
