/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SettingNotifyInt.h"

void CSettingNotifyInt::Reset()
{
  const int def = GetDefault();
  if (GetValue() == def && def != 0)
  {
    // Force an intermediate callback by transitioning through 0. SetValue skips
    // callbacks on no-op writes, so without this the caller's OnSettingChanged
    // never fires when the current value already equals the default.
    SetValue(0);
  }
  SetValue(def);
}
