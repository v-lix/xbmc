/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <functional>

#include <EGL/egl.h>

namespace GUI_BACKGROUND_BLEND
{
// Main GUI context only. The callback draws translucent artwork with the
// current GUI projection. Returns false without drawing on the destination
// when the path is unavailable; the caller then uses its ordinary draw path.
bool Draw(unsigned int maxHeight, float depth, const std::function<void()>& draw);
void ReleaseContext(EGLContext context);
} // namespace GUI_BACKGROUND_BLEND
