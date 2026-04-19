/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/Setting.h"

// CSettingInt variant whose Reset() always fires the change callback, even when
// the current value already equals the XML default. Kodi's CSettingInt::SetValue
// skips callbacks on no-op writes, which is normally fine but breaks use cases
// where "Reset the above settings" must trigger side effects (e.g. re-applying
// a settings preset).
class CSettingNotifyInt : public CSettingInt
{
public:
  using CSettingInt::CSettingInt;
  void Reset() override;
};
