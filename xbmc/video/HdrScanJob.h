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

class CHdrScanJob : public CProgressJob
{
public:
  CHdrScanJob(std::string scope, bool scopeIsFolder, bool updateNfo);
  ~CHdrScanJob() override = default;

  static bool HasNfoFiles(const std::string& scope, bool scopeIsFolder);

  bool DoWork() override;
  const char* GetType() const override { return "HdrScanJob"; }
  bool operator==(const CJob* job) const override;

  std::size_t GetScannedCount() const { return m_scanned; }
  std::size_t GetDatabaseUpdatedCount() const { return m_databaseUpdated; }
  std::size_t GetNfoUpdatedCount() const { return m_nfoUpdated; }
  std::size_t GetFailureCount() const { return m_failed; }
  bool WasCancelled() const { return m_cancelled; }

private:
  std::string m_scope;
  bool m_scopeIsFolder{false};
  bool m_updateNfo{false};
  bool m_cancelled{false};
  std::size_t m_scanned{0};
  std::size_t m_databaseUpdated{0};
  std::size_t m_nfoUpdated{0};
  std::size_t m_failed{0};
};
