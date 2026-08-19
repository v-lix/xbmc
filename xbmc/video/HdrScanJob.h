/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/ProgressJob.h"

#include <cstddef>
#include <string>
#include <vector>

class CHdrScanJob : public CProgressJob
{
public:
  //! \brief A library entry whose HDR metadata still has to be read from the file.
  struct Candidate
  {
    int idFile{-1};
    std::string path;
  };

  //! \brief The outcome of searching the library for entries to rescan.
  struct Collection
  {
    bool queried{false}; //!< the library could be read
    bool cancelled{false}; //!< the user abandoned the search
    bool hasNfoFiles{false}; //!< at least one candidate has an NFO that could be updated
    std::vector<Candidate> candidates;
  };

  CHdrScanJob(std::vector<Candidate> candidates, bool updateNfo);
  ~CHdrScanJob() override = default;

  /*! \brief Search the library for the entries in scope that need rescanning.

   Runs off-thread behind a busy dialog, because looking for the NFO of every candidate
   touches the file system and is slow on network storage.

   \param scope a file, or a folder whose entries are collected recursively
   \param scopeIsFolder whether scope is a folder
   \return what was found, and whether the search ran to completion at all
   */
  static Collection Collect(const std::string& scope, bool scopeIsFolder);

  bool DoWork() override;
  const char* GetType() const override { return "HdrScanJob"; }
  bool operator==(const CJob* job) const override;

  std::size_t GetScannedCount() const { return m_scanned; }
  std::size_t GetDatabaseUpdatedCount() const { return m_databaseUpdated; }
  std::size_t GetNfoUpdatedCount() const { return m_nfoUpdated; }
  std::size_t GetFailureCount() const { return m_failed; }
  bool WasCancelled() const { return m_cancelled; }

private:
  std::vector<Candidate> m_candidates;
  bool m_updateNfo{false};
  bool m_cancelled{false};
  std::size_t m_scanned{0};
  std::size_t m_databaseUpdated{0};
  std::size_t m_nfoUpdated{0};
  std::size_t m_failed{0};
};
