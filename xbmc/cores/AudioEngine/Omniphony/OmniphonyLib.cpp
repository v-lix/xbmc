/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OmniphonyLib.h"

#include "filesystem/SpecialProtocol.h"
#include "utils/log.h"

#include <mutex>
#include <vector>

#if defined(TARGET_POSIX)
#include <dlfcn.h>
#endif

namespace ActiveAE
{

namespace
{

OmniphonyApi g_api;
std::string g_error;
bool g_loaded = false;
std::once_flag g_once;
void* g_handle = nullptr;

/*!
 * \brief The library file name, which carries the ABI major version.
 *
 * The engine derives its Linux soname from ORENDER_ABI_MAJOR, so an
 * incompatible engine has a different file name and is never even opened.
 * Built from the same macro here rather than spelled out, so that the day the
 * major moves this follows it. The runtime handshake in TryCandidate() still
 * runs: it is the only protection on platforms whose file name does not
 * encode the version.
 */
#define OMNIPHONY_STR_(x) #x
#define OMNIPHONY_STR(x) OMNIPHONY_STR_(x)

#if defined(TARGET_DARWIN)
constexpr const char* LIB_NAME = "liborender.dylib";
#elif defined(TARGET_WINDOWS)
constexpr const char* LIB_NAME = "orender.dll";
#else
constexpr const char* LIB_NAME = "liborender.so." OMNIPHONY_STR(ORENDER_ABI_MAJOR);
#endif

void* OpenLibrary(const std::string& path)
{
#if defined(TARGET_POSIX)
  // RTLD_LOCAL: the engine's symbols must not leak into the global namespace,
  // where they could collide with a differently-versioned copy.
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#else
  return nullptr;
#endif
}

void* ResolveSymbol(void* handle, const char* name)
{
#if defined(TARGET_POSIX)
  return dlsym(handle, name);
#else
  return nullptr;
#endif
}

void CloseLibrary(void* handle)
{
#if defined(TARGET_POSIX)
  dlclose(handle);
#endif
}

std::string LastLoaderError()
{
#if defined(TARGET_POSIX)
  const char* err = dlerror();
  return err ? err : "unknown error";
#else
  return "runtime loading is not implemented on this platform";
#endif
}

} // unnamed namespace

bool COmniphonyLib::TryCandidate(const std::string& path)
{
  void* handle = OpenLibrary(path);
  if (!handle)
  {
    CLog::Log(LOGDEBUG, "Omniphony: {} - {}", path, LastLoaderError());
    return false;
  }

  // Step 1: the version handshake must exist at all. A library without it
  // predates the ABI contract and cannot be reasoned about.
  auto versionMajor =
      reinterpret_cast<uint32_t (*)()>(ResolveSymbol(handle, "orender_version_major"));
  if (!versionMajor)
  {
    CLog::Log(LOGWARNING, "Omniphony: {} has no version handshake, rejected", path);
    CloseLibrary(handle);
    return false;
  }

  // Step 2: a differing major means incompatible signatures or struct layouts.
  // OrenderConfig in particular crosses the boundary by layout with no size
  // handshake, so loading a mismatched engine would corrupt memory.
  const uint32_t major = versionMajor();
  if (major != ORENDER_ABI_MAJOR)
  {
    CLog::Log(LOGWARNING, "Omniphony: {} is ABI major {}, this build needs {}, rejected", path,
              major, ORENDER_ABI_MAJOR);
    CloseLibrary(handle);
    return false;
  }

  OmniphonyApi api{};
  api.abiMajor = major;

  // Step 3: required entry points. Any missing one means the library is not
  // usable and the next candidate should be tried.
  struct Required
  {
    const char* name;
    void** slot;
  };
  const Required required[] = {
      {"orender_create", reinterpret_cast<void**>(&api.create)},
      {"orender_destroy", reinterpret_cast<void**>(&api.destroy)},
      {"orender_process", reinterpret_cast<void**>(&api.process)},
      {"orender_reset", reinterpret_cast<void**>(&api.reset)},
      {"orender_channel_count", reinterpret_cast<void**>(&api.channel_count)},
      {"orender_channel_layout", reinterpret_cast<void**>(&api.channel_layout)},
      {"orender_channel_mapping", reinterpret_cast<void**>(&api.channel_mapping)},
      {"orender_set_channel_mode", reinterpret_cast<void**>(&api.set_channel_mode)},
  };
  for (const auto& sym : required)
  {
    *sym.slot = ResolveSymbol(handle, sym.name);
    if (!*sym.slot)
    {
      CLog::Log(LOGWARNING, "Omniphony: {} lacks required symbol {}, rejected", path, sym.name);
      CloseLibrary(handle);
      return false;
    }
  }

  // Step 4: optional entry points, gated on presence rather than on the minor
  // version. This degrades gracefully in both directions of version skew.
  api.build_id = reinterpret_cast<const char* (*)()>(ResolveSymbol(handle, "orender_build_id"));
  api.set_option = reinterpret_cast<int (*)(OrenderRenderer*, const char*, const char*)>(
      ResolveSymbol(handle, "orender_set_option"));
  api.process_pcm =
      reinterpret_cast<decltype(api.process_pcm)>(ResolveSymbol(handle, "orender_process_pcm"));

  if (auto versionMinor =
          reinterpret_cast<uint32_t (*)()>(ResolveSymbol(handle, "orender_version_minor")))
    api.abiMinor = versionMinor();

  api.path = path;

  g_api = api;
  g_handle = handle;
  g_loaded = true;
  g_error.clear();

  // One line answering "which engine did I actually load", as the ABI
  // documentation asks for. It turns a version mismatch into a grep.
  CLog::Log(LOGINFO, "Omniphony: loaded {} (ABI {}.{}, build {}){}", path, api.abiMajor,
            api.abiMinor, api.build_id ? api.build_id() : "unknown",
            api.process_pcm ? ", direct PCM available" : "");
  return true;
}

void COmniphonyLib::Load(const std::string& explicitPath)
{
  // An explicit path is a deliberate choice by the user. If it does not work,
  // fail loudly instead of silently falling back to some other copy: a typo
  // that quietly loads a different engine is far worse than no engine.
  if (!explicitPath.empty())
  {
    const std::string translated = CSpecialProtocol::TranslatePath(explicitPath);
    if (!TryCandidate(translated))
      g_error = "configured library '" + translated + "' could not be loaded";
    return;
  }

  std::vector<std::string> candidates;
  // Alongside the Kodi binary, then in the installed data tree: where a
  // CoreELEC package or addon would place it.
  candidates.push_back(CSpecialProtocol::TranslatePath("special://xbmcbin/omniphony/") + LIB_NAME);
  candidates.push_back(CSpecialProtocol::TranslatePath("special://xbmc/system/omniphony/") +
                       LIB_NAME);
  // Per-user install, for manual deployment and development.
  candidates.push_back(CSpecialProtocol::TranslatePath("special://home/omniphony/") + LIB_NAME);
  // Finally let the dynamic linker search its own paths.
  candidates.emplace_back(LIB_NAME);

  for (const auto& candidate : candidates)
  {
    if (TryCandidate(candidate))
      return;
  }

  g_error = "no compatible liborender found (searched next to the binary, the system tree, "
            "the user profile and the linker path)";
  CLog::Log(LOGINFO, "Omniphony: {}", g_error);
}

const OmniphonyApi* COmniphonyLib::Get(const std::string& explicitPath)
{
  // Both outcomes are cached: a missing engine must not re-run the search on
  // every stream, and the handle must be resolved exactly once per process.
  std::call_once(g_once, [&explicitPath]() { Load(explicitPath); });
  return g_loaded ? &g_api : nullptr;
}

const std::string& COmniphonyLib::GetError()
{
  return g_error;
}

} // namespace ActiveAE
