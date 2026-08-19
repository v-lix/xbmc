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
#include "filesystem/File.h"
#include "filesystem/StackDirectory.h"
#include "guilib/LocalizeStrings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/XMLUtils.h"
#include "utils/log.h"
#include "video/VideoDatabase.h"
#include "video/VideoInfoScanner.h"
#include "video/tags/VideoTagLoaderNFO.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
struct HdrCandidate
{
  int idFile{-1};
  std::string path;
};

std::string ComparablePath(const std::string& path)
{
  if (URIUtils::IsStack(path))
    return XFILE::CStackDirectory::GetFirstStackedFile(path);

  return path;
}

bool IsInScope(const std::string& path, const std::string& scope, bool scopeIsFolder)
{
  const std::string comparablePath = ComparablePath(path);
  if (!scopeIsFolder)
    return URIUtils::PathEquals(comparablePath, scope, true);

  std::string folder = scope;
  URIUtils::AddSlashAtEnd(folder);
  return URIUtils::PathHasParent(comparablePath, folder) ||
         URIUtils::PathEquals(URIUtils::GetDirectory(comparablePath), folder, true);
}

class CHdrScanDatabase final : public CVideoDatabase
{
public:
  bool GetCandidates(const std::string& scope,
                     bool scopeIsFolder,
                     std::vector<HdrCandidate>& candidates)
  {
    candidates.clear();
    std::set<int> seenFiles;

    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
    const bool scanHdr10Plus =
        settings && settings->GetBool(CSettings::SETTING_MYVIDEOS_EXTRACTHDR10PLUS);

    try
    {
      const std::string query =
          "SELECT files.idFile, path.strPath, files.strFilename, streamdetails.strHdrType, "
          "streamdetails.strHdrTypeAlt, streamdetails.strDvProfile "
          "FROM files JOIN path ON path.idPath = files.idPath "
          "JOIN streamdetails ON streamdetails.idFile = files.idFile "
          "WHERE streamdetails.iStreamType = 0";
      m_pDS->query(query);

      while (!m_pDS->eof())
      {
        const int idFile = m_pDS->fv(0).get_asInt();
        const std::string filePath =
            URIUtils::AddFileToFolder(m_pDS->fv(1).get_asString(), m_pDS->fv(2).get_asString());
        const std::string hdrType = m_pDS->fv(3).get_asString();
        const std::string hdrTypeAlt = m_pDS->fv(4).get_asString();
        const std::string dvProfile = m_pDS->fv(5).get_asString();

        const bool needsDolbyVisionScan =
            StringUtils::EqualsNoCase(hdrType, "dolbyvision") &&
            (dvProfile.empty() || hdrTypeAlt.empty() ||
             StringUtils::EndsWithNoCase(dvProfile, " EL"));
        const bool needsHdr10Scan =
            scanHdr10Plus && StringUtils::EqualsNoCase(hdrType, "hdr10") && hdrTypeAlt.empty();

        if ((needsDolbyVisionScan || needsHdr10Scan) && IsInScope(filePath, scope, scopeIsFolder) &&
            seenFiles.insert(idFile).second)
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

  bool UpdateHdrDetails(int idFile, const CStreamDetails& details)
  {
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

      if (databaseVideoStreams == 1)
      {
        m_pDS->exec(PrepareSQL(
            "UPDATE streamdetails SET strHdrType = '%s', strHdrTypeAlt = '%s', "
            "strDvProfile = '%s' WHERE idFile = %i AND iStreamType = %i",
            details.GetVideoHdrType(1).c_str(), details.GetVideoHdrTypeAlt(1).c_str(),
            details.GetVideoDvProfile(1).c_str(), idFile, static_cast<int>(CStreamDetail::VIDEO)));
        return true;
      }

      bool updated = false;
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
          CLog::Log(LOGWARNING,
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
        updated = true;
      }

      return updated;
    }
    catch (...)
    {
      m_pDS->close();
      CLog::Log(LOGERROR, "{} failed for file id {}", __FUNCTION__, idFile);
      return false;
    }
  }
};

class CHdrNfoFinder final : public CVideoTagLoaderNFO
{
public:
  using CVideoTagLoaderNFO::CVideoTagLoaderNFO;

  std::string Find(const CFileItem& item, bool movieFolder) const
  {
    return FindNFO(item, movieFolder);
  }
};

std::string FindNfoPath(CHdrScanDatabase& database, const std::string& filePath)
{
  VIDEO::SScanSettings scanSettings;
  const std::string lookupPath = URIUtils::GetDirectory(ComparablePath(filePath));
  ADDON::ScraperPtr scraper = database.GetScraperForPath(lookupPath, scanSettings);

  CFileItem item(filePath, false);
  CHdrNfoFinder finder(item, scraper, scanSettings.parent_name);
  return finder.Find(item, scanSettings.parent_name);
}

TiXmlElement* EnsureChild(TiXmlElement* parent, const char* name)
{
  if (!parent)
    return nullptr;

  TiXmlElement* child = parent->FirstChildElement(name);
  if (child)
    return child;

  TiXmlElement element(name);
  TiXmlNode* inserted = parent->InsertEndChild(element);
  return inserted ? inserted->ToElement() : nullptr;
}

TiXmlElement* GetOrCreateVideo(TiXmlElement* streamDetails, int streamIndex)
{
  if (!streamDetails || streamIndex < 1)
    return nullptr;

  TiXmlElement* video = streamDetails->FirstChildElement("video");
  if (!video)
  {
    TiXmlElement element("video");
    TiXmlNode* inserted = streamDetails->InsertEndChild(element);
    video = inserted ? inserted->ToElement() : nullptr;
  }

  for (int index = 1; video && index < streamIndex; ++index)
  {
    TiXmlElement* next = video->NextSiblingElement("video");
    if (!next)
    {
      TiXmlElement element("video");
      TiXmlNode* inserted = streamDetails->InsertEndChild(element);
      next = inserted ? inserted->ToElement() : nullptr;
    }
    video = next;
  }

  return video;
}

void SetOrRemoveString(TiXmlElement* parent, const char* name, const std::string& value)
{
  if (!parent)
    return;

  if (value.empty())
  {
    TiXmlElement* child = parent->FirstChildElement(name);
    while (child)
    {
      TiXmlElement* next = child->NextSiblingElement(name);
      parent->RemoveChild(child);
      child = next;
    }
    return;
  }

  XMLUtils::SetString(parent, name, value);

  TiXmlElement* first = parent->FirstChildElement(name);
  if (!first)
    return;

  TiXmlElement* duplicate = first->NextSiblingElement(name);
  while (duplicate)
  {
    TiXmlElement* next = duplicate->NextSiblingElement(name);
    parent->RemoveChild(duplicate);
    duplicate = next;
  }
}

bool PatchNfo(const std::string& nfoPath, const CStreamDetails& details)
{
  if (nfoPath.empty() || details.GetVideoStreamCount() == 0)
    return false;

  CXBMCTinyXML document;
  if (!document.LoadFile(nfoPath))
  {
    CLog::Log(LOGWARNING, "{}: Unable to read NFO {}", __FUNCTION__, nfoPath);
    return false;
  }

  bool patched = false;
  for (TiXmlElement* root = document.RootElement(); root; root = root->NextSiblingElement())
  {
    TiXmlElement* fileInfo = EnsureChild(root, "fileinfo");
    TiXmlElement* streamDetails = EnsureChild(fileInfo, "streamdetails");
    if (!streamDetails)
      continue;

    for (int stream = 1; stream <= details.GetVideoStreamCount(); ++stream)
    {
      TiXmlElement* video = GetOrCreateVideo(streamDetails, stream);
      if (!video)
        continue;

      SetOrRemoveString(video, "hdrtype", details.GetVideoHdrType(stream));
      SetOrRemoveString(video, "hdrtypealt", details.GetVideoHdrTypeAlt(stream));
      SetOrRemoveString(video, "dvprofile", details.GetVideoDvProfile(stream));
      patched = true;
    }
  }

  if (!patched)
    return false;

  const std::string backupPath = nfoPath + ".hdrscan.bak";
  XFILE::CFile::Delete(backupPath);
  if (!XFILE::CFile::Copy(nfoPath, backupPath))
  {
    CLog::Log(LOGWARNING, "{}: Unable to create NFO backup {}", __FUNCTION__, backupPath);
    return false;
  }

  if (document.SaveFile(nfoPath))
  {
    XFILE::CFile::Delete(backupPath);
    return true;
  }

  CLog::Log(LOGWARNING, "{}: Unable to update NFO {}; restoring backup", __FUNCTION__, nfoPath);
  XFILE::CFile::Delete(nfoPath);
  if (XFILE::CFile::Copy(backupPath, nfoPath))
  {
    XFILE::CFile::Delete(backupPath);
  }
  else
  {
    CLog::Log(LOGERROR, "{}: Unable to restore NFO {}; backup remains at {}", __FUNCTION__,
              nfoPath, backupPath);
  }

  return false;
}
} // unnamed namespace

CHdrScanJob::CHdrScanJob(std::string scope, bool scopeIsFolder, bool updateNfo)
  : m_scope(std::move(scope)), m_scopeIsFolder(scopeIsFolder), m_updateNfo(updateNfo)
{
}

bool CHdrScanJob::HasNfoFiles(const std::string& scope, bool scopeIsFolder)
{
  CHdrScanDatabase database;
  if (!database.Open())
    return false;

  std::vector<HdrCandidate> candidates;
  if (!database.GetCandidates(scope, scopeIsFolder, candidates))
  {
    database.Close();
    return false;
  }

  for (const auto& candidate : candidates)
  {
    if (!FindNfoPath(database, candidate.path).empty())
    {
      database.Close();
      return true;
    }
  }

  database.Close();
  return false;
}

bool CHdrScanJob::DoWork()
{
  CHdrScanDatabase database;
  if (!database.Open())
  {
    ++m_failed;
    return false;
  }

  std::vector<HdrCandidate> candidates;
  if (!database.GetCandidates(m_scope, m_scopeIsFolder, candidates))
  {
    database.Close();
    ++m_failed;
    return false;
  }

  SetTitle(g_localizeStrings.Get(102));

  for (std::size_t index = 0; index < candidates.size(); ++index)
  {
    if (IsCancelled())
    {
      m_cancelled = true;
      break;
    }

    const HdrCandidate& candidate = candidates[index];
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
    if (!database.UpdateHdrDetails(candidate.idFile, details))
    {
      ++m_failed;
      continue;
    }

    ++m_databaseUpdated;

    if (m_updateNfo)
    {
      const std::string nfoPath = FindNfoPath(database, candidate.path);
      if (!nfoPath.empty())
      {
        if (PatchNfo(nfoPath, details))
          ++m_nfoUpdated;
        else
          ++m_failed;
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
  return hdrScanJob && m_scope == hdrScanJob->m_scope &&
         m_scopeIsFolder == hdrScanJob->m_scopeIsFolder && m_updateNfo == hdrScanJob->m_updateNfo;
}
